#define _DEFAULT_SOURCE

#include "lsp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "ast.h"
#include "json.h"
#include "lexer.h"

/*
 * Supported: full-document sync, diagnostics, hover, go-to-definition and
 * completion. Each open document keeps its latest text and analysis; every
 * edit re-analyzes the whole document, which is cheap for a single file.
 *
 * The files a document imports are read from disk, so what an editor has
 * not saved is not seen through an import. Saving any document re-analyzes
 * all of them, which brings the importers of a changed file up to date.
 *
 * LSP positions count UTF-16 code units, while the compiler counts bytes, so
 * every position crossing the protocol boundary is converted against the
 * document text.
 */

enum {
    LSP_PARSE_ERROR = -32700,
    LSP_METHOD_NOT_FOUND = -32601,
    LSP_COMPLETION_FUNCTION = 3,
    LSP_COMPLETION_VARIABLE = 6,
    LSP_COMPLETION_MODULE = 9,
    LSP_COMPLETION_ENUM = 13,
    LSP_COMPLETION_KEYWORD = 14,
    LSP_COMPLETION_ENUM_VALUE = 20,
    LSP_COMPLETION_STRUCT = 22,
    LSP_COMPLETION_FILE = 17,
    LSP_COMPLETION_CONSTANT = 21,
    /* Guards against allocating for a corrupt Content-Length. */
    LSP_MAX_MESSAGE = 1 << 28,
};

typedef struct {
    char *uri;
    char *path; /* the file the URI names; NULL if it is not a file */
    char *text;
    size_t text_len;
    Unit unit;
} Doc;

typedef struct {
    Doc *docs;
    size_t doc_count;
    size_t doc_cap;
    bool shutdown_requested;
} Server;

/* ---- Transport ---- */

static bool read_message(Arena *arena, Str *body) {
    long length = -1;
    char line[512];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return false;
        if (line[0] == '\r' || line[0] == '\n') break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            length = strtol(line + 15, NULL, 10);
        }
    }
    if (length < 0 || length > LSP_MAX_MESSAGE) return false;
    char *data = arena_alloc(arena, (size_t)length);
    if (fread(data, 1, (size_t)length, stdin) != (size_t)length) return false;
    *body = (Str){data, (size_t)length};
    return true;
}

static void send_message(const Buf *buf) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", buf->len);
    fwrite(buf->data, 1, buf->len, stdout);
    fflush(stdout);
}

/* Starts a response; the caller appends the result value and then "}". */
static Buf begin_result(Arena *arena, const Json *id) {
    Buf buf = {.arena = arena};
    buf_puts(&buf, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_json_scalar(&buf, id);
    buf_puts(&buf, ",\"result\":");
    return buf;
}

static void send_result(Buf *buf) {
    buf_puts(buf, "}");
    send_message(buf);
}

static void send_null_result(Arena *arena, const Json *id) {
    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "null");
    send_result(&buf);
}

static void send_error(Arena *arena, const Json *id, int code, const char *message) {
    Buf buf = {.arena = arena};
    buf_puts(&buf, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_json_scalar(&buf, id);
    buf_printf(&buf, ",\"error\":{\"code\":%d,\"message\":", code);
    buf_json_string(&buf, (Str){message, strlen(message)});
    buf_puts(&buf, "}}");
    send_message(&buf);
}

/* ---- Position conversion ---- */

static size_t utf16_units(Str src, size_t from, size_t to) {
    size_t units = 0;
    for (size_t i = from; i < to && i < src.len; i++) {
        unsigned char c = (unsigned char)src.data[i];
        if ((c & 0xC0) == 0x80) continue; /* continuation byte */
        units += (c >= 0xF0) ? 2 : 1;     /* astral characters use a pair */
    }
    return units;
}

/* Positions past the end of a line or of the document clamp to its end. */
static size_t position_to_offset(Str src, long line, long character) {
    size_t offset = 0;
    for (long l = 0; l < line; l++) {
        const char *nl = memchr(src.data + offset, '\n', src.len - offset);
        if (!nl) return src.len;
        offset = (size_t)(nl - src.data) + 1;
    }
    long units = 0;
    while (offset < src.len && src.data[offset] != '\n' && units < character) {
        unsigned char c = (unsigned char)src.data[offset];
        units += (c >= 0xF0) ? 2 : 1;
        offset++;
        while (offset < src.len && ((unsigned char)src.data[offset] & 0xC0) == 0x80) {
            offset++;
        }
    }
    return offset;
}

static void write_range(Buf *buf, Str src, SrcLoc loc) {
    size_t line_start = loc.offset - (size_t)(loc.col - 1);
    size_t start = utf16_units(src, line_start, loc.offset);
    size_t end = start + utf16_units(src, loc.offset, loc.offset + loc.len);
    buf_printf(buf,
               "{\"start\":{\"line\":%d,\"character\":%zu},"
               "\"end\":{\"line\":%d,\"character\":%zu}}",
               loc.line - 1, start, loc.line - 1, end);
}

/* Writes the range from the start of one token to the end of another, which
   may be on a later line. */
static void write_span(Buf *buf, Str src, SrcLoc first, SrcLoc last) {
    size_t first_line_start = first.offset - (size_t)(first.col - 1);
    size_t last_line_start = last.offset - (size_t)(last.col - 1);
    buf_printf(buf,
               "{\"start\":{\"line\":%d,\"character\":%zu},"
               "\"end\":{\"line\":%d,\"character\":%zu}}",
               first.line - 1, utf16_units(src, first_line_start, first.offset),
               last.line - 1,
               utf16_units(src, last_line_start, last.offset + last.len));
}

/* ---- Documents ---- */

static Doc *find_doc(Server *server, Str uri) {
    for (size_t i = 0; i < server->doc_count; i++) {
        if (str_eq_cstr(uri, server->docs[i].uri)) return &server->docs[i];
    }
    return NULL;
}

static char *copy_cstr(Str s) {
    char *copy = xmalloc(s.len + 1);
    memcpy(copy, s.data, s.len);
    copy[s.len] = '\0';
    return copy;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The path a "file://" URI names, from malloc, or NULL for any other URI. */
static char *uri_to_path(Str uri) {
    static const char scheme[] = "file://";
    size_t skip = sizeof scheme - 1;
    if (uri.len <= skip || memcmp(uri.data, scheme, skip) != 0) return NULL;
    char *path = xmalloc(uri.len - skip + 1);
    size_t len = 0;
    for (size_t i = skip; i < uri.len; i++) {
        char c = uri.data[i];
        if (c == '%' && i + 2 < uri.len && hex_value(uri.data[i + 1]) >= 0 &&
            hex_value(uri.data[i + 2]) >= 0) {
            c = (char)(hex_value(uri.data[i + 1]) * 16 + hex_value(uri.data[i + 2]));
            i += 2;
        }
        path[len++] = c;
    }
    path[len] = '\0';
    return path;
}

/* Writes the "file://" URI of a path as a JSON string. */
static void write_path_uri(Buf *buf, const char *path) {
    buf_puts(buf, "\"file://");
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || strchr("/-._~", c);
        if (plain) {
            buf_append(buf, p, 1);
        } else {
            buf_printf(buf, "%%%02X", c);
        }
    }
    buf_puts(buf, "\"");
}

static void publish_diagnostics(Arena *arena, Str uri, const Unit *unit) {
    Buf buf = {.arena = arena};
    buf_puts(&buf, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                   "\"params\":{\"uri\":");
    buf_json_string(&buf, uri);
    buf_puts(&buf, ",\"diagnostics\":[");
    for (size_t i = 0; unit && i < unit->diags.count; i++) {
        const Diag *diag = &unit->diags.items[i];
        if (i) buf_puts(&buf, ",");
        buf_puts(&buf, "{\"range\":");
        if (diag->unnecessary) {
            write_span(&buf, unit->src, diag->loc, diag->end);
        } else {
            write_range(&buf, unit->src, diag->loc);
        }
        buf_printf(&buf, ",\"severity\":%d,\"source\":\"wind\",\"message\":",
                   diag->severity == SEV_ERROR ? 1 : 2);
        buf_json_string(&buf, (Str){diag->message, strlen(diag->message)});
        /* Tag 1 is "unnecessary": clients fade the code out. */
        if (diag->unnecessary) buf_puts(&buf, ",\"tags\":[1]");
        buf_puts(&buf, "}");
    }
    buf_puts(&buf, "]}}");
    send_message(&buf);
}

static void set_document(Server *server, Arena *arena, Str uri, Str text) {
    Doc *doc = find_doc(server, uri);
    if (doc) {
        unit_release(&doc->unit);
        free(doc->text);
    } else {
        if (server->doc_count == server->doc_cap) {
            server->doc_cap = server->doc_cap ? server->doc_cap * 2 : 8;
            server->docs = xrealloc(server->docs, server->doc_cap * sizeof(Doc));
        }
        doc = &server->docs[server->doc_count++];
        doc->uri = copy_cstr(uri);
        doc->path = uri_to_path(uri);
    }
    doc->text = copy_cstr(text);
    doc->text_len = text.len;
    unit_analyze(&doc->unit, (Str){doc->text, doc->text_len}, doc->path);
    publish_diagnostics(arena, uri, &doc->unit);
}

/* Analyzes every open document again, for when files they import may have
   changed on disk. */
static void refresh_documents(Server *server, Arena *arena) {
    for (size_t i = 0; i < server->doc_count; i++) {
        Doc *doc = &server->docs[i];
        unit_release(&doc->unit);
        unit_analyze(&doc->unit, (Str){doc->text, doc->text_len}, doc->path);
        publish_diagnostics(arena, (Str){doc->uri, strlen(doc->uri)}, &doc->unit);
    }
}

static void close_document(Server *server, Arena *arena, Str uri) {
    Doc *doc = find_doc(server, uri);
    if (!doc) return;
    unit_release(&doc->unit);
    free(doc->text);
    free(doc->uri);
    free(doc->path);
    *doc = server->docs[--server->doc_count];
    /* Clears the diagnostics the client is still showing for the file. */
    publish_diagnostics(arena, uri, NULL);
}

/* ---- Language features ---- */

/* The cursor counts as on a name when it is inside it or directly after it. */
static bool loc_contains(SrcLoc loc, size_t offset) {
    return offset >= loc.offset && offset <= loc.offset + loc.len;
}

/* Finds the symbol named at offset, whether at its declaration or at a use.
   On success *at is the location of the name under the cursor. */
static const Symbol *symbol_at(const Unit *unit, size_t offset, SrcLoc *at) {
    for (size_t i = 0; i < unit->ref_count; i++) {
        if (loc_contains(unit->refs[i].loc, offset)) {
            *at = unit->refs[i].loc;
            return unit->refs[i].symbol;
        }
    }
    for (size_t i = 0; i < unit->symbols.count; i++) {
        if (loc_contains(unit->symbols.items[i]->loc, offset)) {
            *at = unit->symbols.items[i]->loc;
            return unit->symbols.items[i];
        }
    }
    return NULL;
}

/* Resolves the request's textDocument/position to a document and offset. */
static Doc *request_target(Server *server, const Json *params, size_t *offset) {
    Str uri;
    long line, character;
    const Json *position = json_get(params, "position");
    if (!json_string(json_get(json_get(params, "textDocument"), "uri"), &uri) ||
        !json_integer(json_get(position, "line"), &line) ||
        !json_integer(json_get(position, "character"), &character)) {
        return NULL;
    }
    Doc *doc = find_doc(server, uri);
    if (doc) {
        *offset = position_to_offset(doc->unit.src, line, character);
        /* Answering names the document's types. */
        types_use(&doc->unit.types);
    }
    return doc;
}

/* Writes the symbol's declaration the way it would be spelled in source. */
static void describe_symbol(Buf *buf, const Symbol *symbol) {
    int len = (int)symbol->name.len;
    const char *name = symbol->name.data;
    Str src = symbol->unit->src;
    if (symbol->exported) buf_puts(buf, "export ");
    switch (symbol->kind) {
    case SYM_MODULE: {
        Str path = symbol->decl->as.import.path;
        buf_printf(buf, "import \"%.*s\" as %.*s", (int)path.len, path.data, len, name);
        break;
    }
    case SYM_ENUM_VALUE:
        buf_printf(buf, "%s %.*s = %lld", type_name(symbol->type), len, name, symbol->value);
        break;
    case SYM_TYPE: {
        const TypeInfo *info = type_info(symbol->type);
        const Stmt *decl = symbol->decl;
        if (decl->as.type.kind == TYPE_DECL_ALIAS || !info) {
            buf_printf(buf, "typedef %s %.*s", type_name(symbol->type), len, name);
        } else if (info->is_enum) {
            buf_printf(buf, "enum %s { ", info->short_name);
            for (size_t i = 0; i < info->value_count; i++) {
                buf_printf(buf, "%s%.*s", i ? ", " : "", (int)info->values[i].name.len,
                           info->values[i].name.data);
            }
            buf_puts(buf, " }");
        } else {
            buf_printf(buf, "struct %s { ", info->short_name);
            for (size_t i = 0; i < info->field_count; i++) {
                buf_printf(buf, "%s%s %.*s", i ? "; " : "", type_name(info->fields[i].type),
                           (int)info->fields[i].name.len, info->fields[i].name.data);
            }
            buf_puts(buf, " }");
        }
        break;
    }
    case SYM_CONST: {
        const Expr *init = symbol->decl->as.decl.init;
        buf_printf(buf, "const %s %.*s", type_name(symbol->type), len, name);
        if (symbol->is_array) {
            buf_printf(buf, "[%zu]", symbol->length);
            break;
        }
        buf_puts(buf, " = ");
        buf_append(buf, src.data + init->loc.offset, init->loc.len);
        break;
    }
    case SYM_VAR:
    case SYM_PARAM:
        buf_printf(buf, "%s %.*s", type_name(symbol->type), len, name);
        if (symbol->is_array) buf_printf(buf, "[%zu]", symbol->length);
        if (symbol->is_slice) buf_puts(buf, "[]");
        break;
    case SYM_LABEL:
        buf_printf(buf, "%.*s:", len, name);
        break;
    case SYM_FUNC: {
        const Stmt *func = symbol->decl;
        buf_printf(buf, "%s %.*s(", type_name(symbol->type), len, name);
        for (size_t i = 0; i < func->as.func.param_count; i++) {
            const Param *param = &func->as.func.params[i];
            buf_printf(buf, "%s%s %.*s%s", i ? ", " : "", type_name(param->type),
                       (int)param->name.len, param->name.data, param->is_slice ? "[]" : "");
        }
        buf_puts(buf, ")");
        break;
    }
    }
}

static void handle_hover(Server *server, Arena *arena, const Json *id,
                         const Json *params) {
    size_t offset;
    SrcLoc at;
    Doc *doc = request_target(server, params, &offset);
    const Symbol *symbol = doc ? symbol_at(&doc->unit, offset, &at) : NULL;
    if (!symbol) {
        send_null_result(arena, id);
        return;
    }

    Buf text = {.arena = arena};
    buf_puts(&text, "```wind\n");
    describe_symbol(&text, symbol);
    buf_puts(&text, "\n```");

    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    buf_json_string(&buf, (Str){text.data, text.len});
    buf_puts(&buf, "},\"range\":");
    write_range(&buf, doc->unit.src, at);
    buf_puts(&buf, "}");
    send_result(&buf);
}

static void handle_definition(Server *server, Arena *arena, const Json *id,
                              const Json *params) {
    size_t offset;
    SrcLoc at;
    Doc *doc = request_target(server, params, &offset);
    const Symbol *symbol = doc ? symbol_at(&doc->unit, offset, &at) : NULL;
    if (!symbol) {
        send_null_result(arena, id);
        return;
    }
    /* A module's name leads to the file it stands for; anything else to its
       declaration, which may be in an imported file. */
    const Unit *target = symbol->unit;
    SrcLoc loc = symbol->loc;
    if (symbol->kind == SYM_MODULE && symbol->module) {
        target = symbol->module;
        loc = (SrcLoc){.line = 1, .col = 1};
    }
    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "{\"uri\":");
    if (target == &doc->unit) {
        buf_json_string(&buf, (Str){doc->uri, strlen(doc->uri)});
    } else {
        write_path_uri(&buf, target->real_path);
    }
    buf_puts(&buf, ",\"range\":");
    write_range(&buf, target->src, loc);
    buf_puts(&buf, "}");
    send_result(&buf);
}

/* The function whose body contains offset, or NULL for top-level code. */
static const Stmt *function_at(const Program *program, size_t offset) {
    for (size_t i = 0; i < program->top.count; i++) {
        const Stmt *stmt = program->top.stmts[i];
        if (stmt->kind == STMT_FUNC && offset > stmt->as.func.body.open.offset &&
            offset <= stmt->as.func.body.close.offset) {
            return stmt;
        }
    }
    return NULL;
}

/* True if the symbol's name can be used at offset. Functions are visible
   everywhere, and what is declared directly at the top level reaches into
   the functions below it. */
static bool visible_at(const Symbol *symbol, size_t offset, const Stmt *func) {
    if (symbol->kind == SYM_LABEL) return false;
    if (symbol->kind == SYM_FUNC || symbol->kind == SYM_MODULE ||
        symbol->kind == SYM_TYPE || symbol->kind == SYM_ENUM_VALUE) {
        return true;
    }
    if (offset < symbol->visible_from || offset > symbol->visible_to) return false;
    return symbol->func == func || symbol->global;
}

static int completion_kind(const Symbol *symbol) {
    switch (symbol->kind) {
    case SYM_FUNC:   return LSP_COMPLETION_FUNCTION;
    case SYM_CONST:  return LSP_COMPLETION_CONSTANT;
    case SYM_MODULE: return LSP_COMPLETION_MODULE;
    case SYM_ENUM_VALUE: return LSP_COMPLETION_ENUM_VALUE;
    case SYM_TYPE:
        return type_is_enum(symbol->type) ? LSP_COMPLETION_ENUM : LSP_COMPLETION_STRUCT;
    default:         return LSP_COMPLETION_VARIABLE;
    }
}

static void write_completion(Buf *buf, Arena *arena, const Symbol *symbol) {
    Buf detail = {.arena = arena};
    describe_symbol(&detail, symbol);
    buf_puts(buf, "{\"label\":");
    buf_json_string(buf, symbol->name);
    buf_printf(buf, ",\"kind\":%d,\"detail\":", completion_kind(symbol));
    buf_json_string(buf, (Str){detail.data, detail.len});
    buf_puts(buf, "}");
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

/* If the text before offset reads "module." followed by the start of a
   name, returns the module it names; NULL otherwise. */
static const Symbol *module_before(const Unit *unit, size_t offset) {
    Str src = unit->src;
    size_t end = offset;
    while (end > 0 && is_name_char(src.data[end - 1])) end--;
    if (end == 0 || src.data[end - 1] != '.') return NULL;
    end--;
    size_t start = end;
    while (start > 0 && is_name_char(src.data[start - 1])) start--;
    Str name = {src.data + start, end - start};
    for (size_t i = 0; i < unit->symbols.count; i++) {
        const Symbol *symbol = unit->symbols.items[i];
        if (symbol->kind == SYM_MODULE && str_eq(symbol->name, name)) return symbol;
    }
    return NULL;
}

/* If offset is inside the path of an import, as in 'import "lib/sh|', sets
   *typed to the path up to the cursor and returns true. */
static bool import_path_before(Str src, size_t offset, Str *typed) {
    size_t start = offset;
    while (start > 0 && src.data[start - 1] != '\n') start--;
    size_t i = start;
    while (i < offset && (src.data[i] == ' ' || src.data[i] == '\t')) i++;
    if (offset - i < 6 || memcmp(src.data + i, "import", 6) != 0) return false;
    i += 6;
    if (i == offset || (src.data[i] != ' ' && src.data[i] != '\t')) return false;
    while (i < offset && (src.data[i] == ' ' || src.data[i] == '\t')) i++;
    if (i == offset || src.data[i] != '"') return false;
    i++;
    if (memchr(src.data + i, '"', offset - i)) return false;
    *typed = (Str){src.data + i, offset - i};
    return true;
}

enum {
    /* Bounds on the search for importable files, which keep a document that
       sits high in a large tree from stalling the editor. */
    IMPORT_SEARCH_DEPTH = 6,
    IMPORT_SEARCH_ITEMS = 500,
};

typedef struct {
    Buf *buf;
    const Doc *doc;
    SrcLoc typed; /* the path written so far, which every item replaces */
    size_t count;
} ImportSearch;

/* Adds the Wind files in the directory at dir_path, and in the directories
   below it, to the completion list. shown is the same directory as an
   import would spell it: empty, or ending in "/". */
static void list_importable(ImportSearch *search, const char *dir_path, const char *shown,
                            int depth) {
    DIR *entries = opendir(dir_path[0] ? dir_path : ".");
    if (!entries) return;
    for (struct dirent *entry; (entry = readdir(entries));) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (name[0] == '.' || search->count >= IMPORT_SEARCH_ITEMS) continue;

        char *full = xmalloc(strlen(dir_path) + len + 2);
        char *spelled = xmalloc(strlen(shown) + len + 2);
        sprintf(full, "%s%s", dir_path, name);
        sprintf(spelled, "%s%s", shown, name);
        struct stat info;
        if (stat(full, &info) != 0) {
            /* unreadable or a dangling link: not importable */
        } else if (S_ISDIR(info.st_mode)) {
            if (depth < IMPORT_SEARCH_DEPTH) {
                strcat(full, "/");
                strcat(spelled, "/");
                list_importable(search, full, spelled, depth + 1);
            }
        } else if (S_ISREG(info.st_mode) && len > 5 && strcmp(name + len - 5, ".wind") == 0) {
            /* A file cannot import itself. */
            char *real_path = path_canonical(full);
            const char *own = search->doc->unit.real_path;
            if (!real_path || !own || strcmp(real_path, own) != 0) {
                Buf *buf = search->buf;
                Str text = {spelled, strlen(spelled)};
                if (search->count++) buf_puts(buf, ",");
                buf_puts(buf, "{\"label\":");
                buf_json_string(buf, text);
                buf_printf(buf, ",\"kind\":%d,\"filterText\":", LSP_COMPLETION_FILE);
                buf_json_string(buf, text);
                buf_puts(buf, ",\"textEdit\":{\"range\":");
                write_range(buf, search->doc->unit.src, search->typed);
                buf_puts(buf, ",\"newText\":");
                buf_json_string(buf, text);
                buf_puts(buf, "}}");
            }
            free(real_path);
        }
        free(full);
        free(spelled);
    }
    closedir(entries);
}

/* Completes the path of an import with every Wind file at or below the
   directory the path so far points into, each as the whole path to write.
   An item replaces all of what was typed, so that the client need not agree
   with the server on what a word is inside a string. */
static void complete_import_path(Buf *buf, const Doc *doc, size_t offset, Str typed) {
    size_t dir_len = typed.len;
    while (dir_len > 0 && typed.data[dir_len - 1] != '/') dir_len--;

    ImportSearch search = {.buf = buf, .doc = doc};
    search.typed = (SrcLoc){.offset = offset - typed.len, .len = typed.len, .line = 1, .col = 1};
    for (size_t i = 0; i < search.typed.offset; i++) {
        search.typed.col++;
        if (doc->unit.src.data[i] == '\n') {
            search.typed.line++;
            search.typed.col = 1;
        }
    }

    char *shown = xmalloc(dir_len + 1);
    memcpy(shown, typed.data, dir_len);
    shown[dir_len] = '\0';
    char *dir_path = path_beside(doc->path, (Str){typed.data, dir_len});
    list_importable(&search, dir_path, shown, 0);
    free(dir_path);
    free(shown);
}

static void handle_completion(Server *server, Arena *arena, const Json *id,
                              const Json *params) {
    size_t offset = 0;
    Doc *doc = request_target(server, params, &offset);

    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "[");
    Str typed;
    if (doc && import_path_before(doc->unit.src, offset, &typed)) {
        complete_import_path(&buf, doc, offset, typed);
        buf_puts(&buf, "]");
        send_result(&buf);
        return;
    }
    const Symbol *module = doc ? module_before(&doc->unit, offset) : NULL;
    if (module) {
        /* After "module." only what the module exports can follow. */
        bool first = true;
        for (size_t i = 0; module->module && i < module->module->symbols.count; i++) {
            const Symbol *symbol = module->module->symbols.items[i];
            if (!symbol->exported) continue;
            if (!first) buf_puts(&buf, ",");
            write_completion(&buf, arena, symbol);
            first = false;
        }
        buf_puts(&buf, "]");
        send_result(&buf);
        return;
    }
    for (size_t i = 0; i < keyword_count(); i++) {
        if (i) buf_puts(&buf, ",");
        buf_printf(&buf, "{\"label\":\"%s\",\"kind\":%d}", keyword_name(i),
                   LSP_COMPLETION_KEYWORD);
    }
    const Stmt *func = doc ? function_at(&doc->unit.program, offset) : NULL;
    for (size_t i = 0; doc && i < doc->unit.symbols.count; i++) {
        const Symbol *symbol = doc->unit.symbols.items[i];
        if (!visible_at(symbol, offset, func)) continue;
        buf_puts(&buf, ",");
        write_completion(&buf, arena, symbol);
    }
    buf_puts(&buf, "]");
    send_result(&buf);
}

static void handle_initialize(Arena *arena, const Json *id) {
    Buf buf = begin_result(arena, id);
    buf_puts(&buf,
             "{\"capabilities\":{"
             "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":true},"
             "\"hoverProvider\":true,"
             "\"definitionProvider\":true,"
             "\"completionProvider\":{\"triggerCharacters\":[\".\",\"\\\"\",\"/\"]}},"
             "\"serverInfo\":{\"name\":\"wind\",\"version\":\"" WIND_VERSION "\"}}");
    send_result(&buf);
}

/* ---- Dispatch ---- */

static bool method_is(Str method, const char *name) {
    return str_eq_cstr(method, name);
}

/* Returns false once the client has asked the server to exit. */
static bool dispatch(Server *server, Arena *arena, const Json *message) {
    const Json *id = json_get(message, "id");
    const Json *params = json_get(message, "params");
    Str method;
    /* A message without a method is a response to the server; none are
       expected, since the server sends no requests. */
    if (!json_string(json_get(message, "method"), &method)) return true;

    const Json *document = json_get(params, "textDocument");
    Str uri, text;

    if (method_is(method, "initialize")) {
        handle_initialize(arena, id);
    } else if (method_is(method, "shutdown")) {
        server->shutdown_requested = true;
        send_null_result(arena, id);
    } else if (method_is(method, "exit")) {
        return false;
    } else if (method_is(method, "textDocument/didOpen")) {
        if (json_string(json_get(document, "uri"), &uri) &&
            json_string(json_get(document, "text"), &text)) {
            set_document(server, arena, uri, text);
        }
    } else if (method_is(method, "textDocument/didChange")) {
        /* With full sync, the last change holds the complete new text. */
        const Json *changes = json_get(params, "contentChanges");
        const Json *last = json_at(changes, json_count(changes) - 1);
        if (json_string(json_get(document, "uri"), &uri) &&
            json_string(json_get(last, "text"), &text)) {
            set_document(server, arena, uri, text);
        }
    } else if (method_is(method, "textDocument/didClose")) {
        if (json_string(json_get(document, "uri"), &uri)) {
            close_document(server, arena, uri);
        }
    } else if (method_is(method, "textDocument/didSave")) {
        refresh_documents(server, arena);
    } else if (method_is(method, "textDocument/hover")) {
        handle_hover(server, arena, id, params);
    } else if (method_is(method, "textDocument/definition")) {
        handle_definition(server, arena, id, params);
    } else if (method_is(method, "textDocument/completion")) {
        handle_completion(server, arena, id, params);
    } else if (id) {
        /* Unknown requests must be answered; unknown notifications are
           ignored. */
        send_error(arena, id, LSP_METHOD_NOT_FOUND, "method not supported");
    }
    return true;
}

int lsp_run(void) {
    Server server = {0};
    bool running = true;
    while (running) {
        /* Holds one message and its response. */
        Arena arena = {0};
        Str body;
        if (!read_message(&arena, &body)) {
            arena_release(&arena);
            break;
        }
        const Json *message = json_parse(&arena, body);
        if (message) {
            running = dispatch(&server, &arena, message);
        } else {
            send_error(&arena, NULL, LSP_PARSE_ERROR, "invalid JSON");
        }
        arena_release(&arena);
    }
    for (size_t i = 0; i < server.doc_count; i++) {
        unit_release(&server.docs[i].unit);
        free(server.docs[i].text);
        free(server.docs[i].uri);
        free(server.docs[i].path);
    }
    free(server.docs);

    /* The protocol asks for a failure status if the client never sent
       "shutdown". */
    return server.shutdown_requested ? 0 : 1;
}
