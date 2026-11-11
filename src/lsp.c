#include "lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ast.h"
#include "json.h"
#include "lexer.h"

/*
 * Supported: full-document sync, diagnostics, hover, go-to-definition and
 * completion. Each open document keeps its latest text and analysis; every
 * edit re-analyzes the whole document, which is cheap for a single file.
 *
 * LSP positions count UTF-16 code units, while the compiler counts bytes, so
 * every position crossing the protocol boundary is converted against the
 * document text.
 */

enum {
    LSP_PARSE_ERROR = -32700,
    LSP_METHOD_NOT_FOUND = -32601,
    LSP_COMPLETION_KEYWORD = 14,
    LSP_COMPLETION_CONSTANT = 21,
    /* Guards against allocating for a corrupt Content-Length. */
    LSP_MAX_MESSAGE = 1 << 28,
};

typedef struct {
    char *uri;
    char *text;
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
        write_range(&buf, unit->src, diag->loc);
        buf_printf(&buf, ",\"severity\":%d,\"source\":\"wind\",\"message\":",
                   diag->severity == SEV_ERROR ? 1 : 2);
        buf_json_string(&buf, (Str){diag->message, strlen(diag->message)});
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
    }
    doc->text = copy_cstr(text);
    unit_analyze(&doc->unit, (Str){doc->text, text.len});
    publish_diagnostics(arena, uri, &doc->unit);
}

static void close_document(Server *server, Arena *arena, Str uri) {
    Doc *doc = find_doc(server, uri);
    if (!doc) return;
    unit_release(&doc->unit);
    free(doc->text);
    free(doc->uri);
    *doc = server->docs[--server->doc_count];
    /* Clears the diagnostics the client is still showing for the file. */
    publish_diagnostics(arena, uri, NULL);
}

/* ---- Language features ---- */

/* The cursor counts as on a name when it is inside it or directly after it. */
static bool loc_contains(SrcLoc loc, size_t offset) {
    return offset >= loc.offset && offset <= loc.offset + loc.len;
}

static const Stmt *decl_of_ident(const Expr *expr, size_t offset, SrcLoc *at) {
    if (expr->kind != EXPR_IDENT || !loc_contains(expr->loc, offset)) return NULL;
    *at = expr->loc;
    return expr->as.ident.decl;
}

/* Finds the constant named at offset, whether at its declaration or at a use.
   On success *at is the location of the name under the cursor. */
static const Stmt *decl_at(const Program *program, size_t offset, SrcLoc *at) {
    for (size_t i = 0; i < program->count; i++) {
        const Stmt *stmt = program->stmts[i];
        const Stmt *decl = NULL;
        if (stmt->kind == STMT_CONST_DECL) {
            if (loc_contains(stmt->loc, offset)) {
                *at = stmt->loc;
                return stmt;
            }
            decl = decl_of_ident(stmt->as.const_decl.init, offset, at);
        } else {
            for (size_t j = 0; j < stmt->as.cout.count && !decl; j++) {
                decl = decl_of_ident(stmt->as.cout.args[j], offset, at);
            }
        }
        if (decl) return decl;
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
    if (doc) *offset = position_to_offset(doc->unit.src, line, character);
    return doc;
}

static void handle_hover(Server *server, Arena *arena, const Json *id,
                         const Json *params) {
    size_t offset;
    SrcLoc at;
    Doc *doc = request_target(server, params, &offset);
    const Stmt *decl = doc ? decl_at(&doc->unit.program, offset, &at) : NULL;
    if (!decl) {
        send_null_result(arena, id);
        return;
    }

    Str src = doc->unit.src;
    Str name = decl->as.const_decl.name;
    Buf text = {.arena = arena};
    buf_printf(&text, "```wind\nconst %s %.*s", type_name(decl->as.const_decl.type),
               (int)name.len, name.data);
    const Expr *literal = const_decl_literal(decl);
    if (literal) {
        buf_puts(&text, " = ");
        buf_append(&text, src.data + literal->loc.offset, literal->loc.len);
    }
    buf_puts(&text, "\n```");

    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    buf_json_string(&buf, (Str){text.data, text.len});
    buf_puts(&buf, "},\"range\":");
    write_range(&buf, src, at);
    buf_puts(&buf, "}");
    send_result(&buf);
}

static void handle_definition(Server *server, Arena *arena, const Json *id,
                              const Json *params) {
    size_t offset;
    SrcLoc at;
    Doc *doc = request_target(server, params, &offset);
    const Stmt *decl = doc ? decl_at(&doc->unit.program, offset, &at) : NULL;
    if (!decl) {
        send_null_result(arena, id);
        return;
    }
    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "{\"uri\":");
    buf_json_string(&buf, (Str){doc->uri, strlen(doc->uri)});
    buf_puts(&buf, ",\"range\":");
    write_range(&buf, doc->unit.src, decl->loc);
    buf_puts(&buf, "}");
    send_result(&buf);
}

static void handle_completion(Server *server, Arena *arena, const Json *id,
                              const Json *params) {
    size_t offset = 0;
    Doc *doc = request_target(server, params, &offset);

    Buf buf = begin_result(arena, id);
    buf_puts(&buf, "[");
    for (size_t i = 0; i < wind_keyword_count; i++) {
        if (i) buf_puts(&buf, ",");
        buf_printf(&buf, "{\"label\":\"%s\",\"kind\":%d}", wind_keywords[i],
                   LSP_COMPLETION_KEYWORD);
    }
    /* Only constants declared above the cursor are in scope. */
    for (size_t i = 0; doc && i < doc->unit.program.count; i++) {
        const Stmt *stmt = doc->unit.program.stmts[i];
        if (stmt->kind != STMT_CONST_DECL || stmt->loc.offset >= offset) continue;
        buf_puts(&buf, ",{\"label\":");
        buf_json_string(&buf, stmt->as.const_decl.name);
        buf_printf(&buf, ",\"kind\":%d,\"detail\":\"const %s\"}",
                   LSP_COMPLETION_CONSTANT, type_name(stmt->as.const_decl.type));
    }
    buf_puts(&buf, "]");
    send_result(&buf);
}

static void handle_initialize(Arena *arena, const Json *id) {
    Buf buf = begin_result(arena, id);
    buf_puts(&buf,
             "{\"capabilities\":{"
             "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
             "\"hoverProvider\":true,"
             "\"definitionProvider\":true,"
             "\"completionProvider\":{}},"
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
    }
    free(server.docs);

    /* The protocol asks for a failure status if the client never sent
       "shutdown". */
    return server.shutdown_requested ? 0 : 1;
}
