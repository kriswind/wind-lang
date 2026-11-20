#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"
#include "targets/target.h"

/*
 * Typing rules:
 *
 * - There are no implicit conversions: both sides of an operator, an
 *   initializer and its variable, an argument and its parameter must have the
 *   same type. T(x) converts explicitly.
 * - Number literals are untyped until used: they take the type the context
 *   asks for (the "hint"), and default to int or float without one.
 *
 * Arrays:
 *
 * - An array is not a value: its name can be indexed, measured with len()
 *   or sizeof, printed, or passed to an array parameter, and anywhere else
 *   it is a pointer to the first element. A Symbol's type is then the type
 *   of the elements.
 *
 * Pointers:
 *
 * - "T *" points to a T. "&place" makes one from a variable, an element, a
 *   field or what another pointer points to; "*p" is what p points to, and
 *   "p->f" and "p.f" both reach a field through p. A pointer that is given
 *   no value is NULL.
 * - A pointer moves by whole elements: "p + n", "p - n", "p[n]", and "p - q"
 *   for how many elements apart two pointers are. None of this is checked
 *   against the bounds of what p points into.
 * - An array's name used as a value is a pointer to its first element,
 *   except for a constant array, which a pointer could change.
 * - A pointer converts only explicitly, to another pointer type or to and
 *   from uintptr.
 * - Where a truth value is asked for (a condition, "!", "&&", "||"), a
 *   pointer is one: true unless it is NULL. It is still not a bool, and
 *   cannot be stored in one.
 *
 * - "char *" is C's string: a pointer to characters that end at a '\0'. A
 *   string literal is one where a "char *" is asked for. It is a different
 *   type from string, which carries its length: string(p) views the
 *   characters p points to as a string, and cstring(s) copies a string
 *   into memory that ends with a '\0'.
 *
 * Unreachable code is reported only where it is certain: after a statement
 * that never continues, and in a body guarded by a literal true or false. A
 * region that holds a label is left alone, since a goto may enter it.
 *
 * Scoping rules:
 *
 * - Names must be declared before use, except functions and labels.
 * - A function sees what is declared directly at the top level: every
 *   function, and the constants and variables declared above it.
 *
 * Declared types:
 *
 * - A struct or an enum is a type of its own, distinct from every other. An
 *   enum converts to and from integers only explicitly; its names are
 *   constants of the enum's type. "typedef" gives a type another name and
 *   makes no new type.
 * - Types are declared at the top level and are known throughout the file.
 *   A struct can be pointed to from anywhere, itself included, but a struct
 *   held by value must be declared above the type that holds it.
 * - "{ ... }" is a struct value whose type comes from where it is used, as
 *   a number literal's does.
 * - typeof(expr) is the type the expression has; the expression is checked
 *   but never run.
 *
 * Modules:
 *
 * - Every file is a module. "import" analyzes another file and binds a name
 *   to it, through which the functions and constants that file marks
 *   "export" are reached as "name.member". Everything else stays private.
 * - A file is analyzed once, however many files import it, and imports may
 *   not form a cycle.
 * - An imported file is not run as a script: without a main it may only hold
 *   declarations, and a main it has is left uncalled. Its top-level
 *   variables are initialized before the program starts.
 * - A path that starts with "::" is in the libraries directory; any other
 *   is beside the importing file. ".wind" may be left off.
 * - A path that names a folder imports every ".wind" file in it and in the
 *   folders below it, as one module: each file is a part of it on an equal
 *   footing, and what any of them exports, the module exports. Two parts
 *   cannot export one name. A path is a file before it is a folder, unless
 *   it ends in "/".
 * - An import of a folder is called what the "#module" of its files says,
 *   else after the folder; an import of a file is called after the file.
 *   "as" gives either another name.
 *
 * External declarations:
 *
 * - "extern" declares a function or a constant that C defines. It has no
 *   body or value here, and keeps its own name in the generated C. Only
 *   types that C has a matching form for can cross: numbers, bool, char,
 *   enums and pointers.
 *
 * Settings:
 *
 * - "#target name" and "#board name" say what a file is compiled for,
 *   above everything else in it. The program's own file chooses its target
 *   and board, unless the command line names others. An imported file can
 *   only state the target it is written for, which the program must then
 *   be built for.
 *
 * The platform (see types.h) decides the sizes of types and which exist,
 * whether main can take the program's arguments, and which function, if
 * any, is called over and over after the program has started.
 */

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    size_t end; /* source offset where the scope closes */
    SymbolList symbols;
};

typedef struct {
    Unit *unit;
    Arena *arena;
    Diags *diags;
    Scope *scope;
    Scope *global;
    /* The function being checked, or NULL in top-level code. */
    Stmt *func;
    SymbolList *locals;
    SymbolList labels;
    /* The innermost loop being checked, or NULL outside any loop. A
       deferred statement starts with none, since it cannot leave. */
    const Stmt *loop;
    /* What a break leaves: the innermost loop or switch, or NULL. */
    const Stmt *breakable;
    /* The "fallthrough" that ends the case being checked, the one place
       one may be. */
    const Stmt *fallthrough_ok;
    /* Inside a deferred statement. */
    bool in_defer;
    /* A growable array or map field may be named here as one, rather than
       read as a value. */
    bool want_array;
    /* The first goto and the first defer in the function being checked, or
       in top-level code, which may not go together. */
    const Stmt *goto_seen;
    const Stmt *defer_seen;
    int next_id;
} Checker;

const char *op_text(Op op) {
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUB: return "-";
    case OP_MUL: return "*";
    case OP_DIV: return "/";
    case OP_MOD: return "%";
    case OP_EQ:  return "==";
    case OP_NE:  return "!=";
    case OP_LT:  return "<";
    case OP_LE:  return "<=";
    case OP_GT:  return ">";
    case OP_GE:  return ">=";
    case OP_AND: return "&&";
    case OP_OR:  return "||";
    case OP_NEG: return "-";
    case OP_NOT: return "!";
    case OP_BIT_AND: return "&";
    case OP_BIT_OR:  return "|";
    case OP_BIT_XOR: return "^";
    case OP_SHL:     return "<<";
    case OP_SHR:     return ">>";
    case OP_BIT_NOT: return "~";
    case OP_ADDR:    return "&";
    case OP_DEREF:   return "*";
    }
    return "?";
}

static void error(Checker *c, SrcLoc loc, const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    diag_add(c->diags, SEV_ERROR, loc, "%s", message);
}

#define NAME(str) (int)(str).len, (str).data

/* ---- Symbols and scopes ---- */

static void list_push(Checker *c, SymbolList *list, Symbol *symbol) {
    ARENA_PUSH(c->arena, list->items, list->count, list->cap, symbol);
}

static Symbol *new_symbol(Checker *c, SymbolKind kind, Str name, SrcLoc loc,
                          TypeKind type) {
    Symbol *symbol = arena_alloc(c->arena, sizeof *symbol);
    memset(symbol, 0, sizeof *symbol);
    symbol->kind = kind;
    symbol->name = name;
    symbol->loc = loc;
    symbol->type = type;
    symbol->id = c->next_id++;
    symbol->unit = c->unit;
    symbol->func = c->func;
    symbol->global = c->scope == c->global;
    symbol->visible_from = loc.offset + loc.len;
    symbol->visible_to = c->scope->end;
    list_push(c, &c->unit->symbols, symbol);
    return symbol;
}

static void add_ref(Checker *c, SrcLoc loc, const Symbol *symbol) {
    Ref ref = {loc, symbol};
    ARENA_PUSH(c->arena, c->unit->refs, c->unit->ref_count, c->unit->ref_cap, ref);
}

static Symbol *find_in(const SymbolList *list, Str name) {
    for (size_t i = 0; i < list->count; i++) {
        if (str_eq(list->items[i]->name, name)) return list->items[i];
    }
    return NULL;
}

static Symbol *lookup(const Checker *c, Str name) {
    for (const Scope *scope = c->scope; scope; scope = scope->parent) {
        Symbol *symbol = find_in(&scope->symbols, name);
        if (symbol) return symbol;
    }
    return NULL;
}

/* Adds a symbol to the innermost scope. A name may shadow one from an outer
   scope but not repeat one from its own. */
static void declare(Checker *c, Symbol *symbol) {
    const Symbol *prev = find_in(&c->scope->symbols, symbol->name);
    if (prev) {
        error(c, symbol->loc, "'%.*s' is already defined at line %d",
              NAME(symbol->name), prev->loc.line);
        /* Spares the duplicate a second, "never used" diagnostic. */
        symbol->used = true;
        return;
    }
    list_push(c, &c->scope->symbols, symbol);
}

static void push_scope(Checker *c, Scope *scope, size_t end) {
    memset(scope, 0, sizeof *scope);
    scope->parent = c->scope;
    scope->end = end;
    c->scope = scope;
}

static void pop_scope(Checker *c) {
    c->scope = c->scope->parent;
}

/* Looks a name up for reading or writing and records the reference.
   Returns NULL, with the problem reported, if it cannot be used here. */
static Str default_module_name(Str path);

static Symbol *resolve(Checker *c, Str name, SrcLoc loc) {
    Symbol *symbol = lookup(c, name);
    if (!symbol) {
        /* An import is named by its file's #module, which may not be what
           its path suggests. */
        const Block *top = &c->unit->program.top;
        for (size_t i = 0; i < top->count; i++) {
            const Stmt *stmt = top->stmts[i];
            const Symbol *module = stmt->kind == STMT_IMPORT ? stmt->as.import.symbol : NULL;
            if (module && str_eq(default_module_name(stmt->as.import.path), name)) {
                error(c, loc, "undefined name '%.*s'; the import of '%.*s' is named '%.*s'",
                      NAME(name), NAME(stmt->as.import.path), NAME(module->name));
                return NULL;
            }
        }
        error(c, loc, "undefined name '%.*s'", NAME(name));
        return NULL;
    }
    add_ref(c, loc, symbol);
    return symbol;
}

static const char *symbol_what(const Symbol *symbol) {
    switch (symbol->kind) {
    case SYM_CONST:  return "constant";
    case SYM_VAR:    return "variable";
    case SYM_PARAM:  return "parameter";
    case SYM_FUNC:   return "function";
    case SYM_LABEL:  return "label";
    case SYM_MODULE: return "module";
    case SYM_TYPE:   return "type";
    case SYM_ENUM_VALUE: return "enum value";
    }
    return "name";
}

/* What a module declares at its top level under a name, exported or not;
   NULL if there is nothing. */
static Symbol *module_member(const Unit *module, Str name) {
    for (size_t i = 0; i < module->symbols.count; i++) {
        Symbol *symbol = module->symbols.items[i];
        bool is_member = symbol->global &&
                         (symbol->kind == SYM_CONST || symbol->kind == SYM_VAR ||
                          symbol->kind == SYM_FUNC || symbol->kind == SYM_TYPE ||
                          symbol->kind == SYM_ENUM_VALUE);
        if (is_member && str_eq(symbol->name, name)) return symbol;
    }
    return NULL;
}

/* Looks up "module.name" and records both references. Returns NULL, with
   the problem reported, if the name cannot be used here. A module that
   failed to load has had that reported at its import, and yields NULL
   quietly. */
/* What an import's parts declare at their top level under a name: an
   exported one if there is one, else any; NULL if there is nothing. */
static Symbol *part_member(const Symbol *owner, Str name) {
    Symbol *found = NULL;
    for (size_t i = 0; i < owner->part_count; i++) {
        Symbol *symbol = module_member(owner->parts[i], name);
        if (symbol && symbol->exported) return symbol;
        if (symbol && !found) found = symbol;
    }
    return found;
}

static Symbol *resolve_member(Checker *c, Str module, SrcLoc module_loc, Str name,
                              SrcLoc name_loc) {
    Symbol *owner = resolve(c, module, module_loc);
    if (!owner) return NULL;
    if (owner->kind != SYM_MODULE) {
        error(c, module_loc, "'%.*s' is a %s, not a module", NAME(module),
              symbol_what(owner));
        return NULL;
    }
    owner->used = true;
    if (!owner->part_count) return NULL;
    Symbol *symbol = part_member(owner, name);
    if (!symbol) {
        error(c, name_loc, "module '%.*s' has no '%.*s'", NAME(module), NAME(name));
        return NULL;
    }
    if (!symbol->exported) {
        error(c, name_loc, "'%.*s' is private to module '%.*s'; mark it 'export' there "
                           "to use it here",
              NAME(name), NAME(module));
        return NULL;
    }
    add_ref(c, name_loc, symbol);
    return symbol;
}

/* ---- Expressions ---- */

static TypeKind check_expr(Checker *c, Expr *expr, TypeKind hint);
static TypeKind check_operand(Checker *c, Expr *expr);
static TypeKind check_value(Checker *c, Expr *expr, TypeKind type);
static const char *quoted(Checker *c, const Expr *expr);
static void mark_changed(Expr *expr);
static const Symbol *place_root(const Expr *expr);
static const Expr *moving_part(const Expr *expr);
static TypeKind as_value(Checker *c, Expr *expr, TypeKind type);
static bool report_shared(Checker *c, const Expr *expr, const char *doing);
static bool type_owns(TypeKind type);
static void check_condition(Checker *c, Expr *cond);
static bool is_format(Builtin builtin);
static Builtin builtin_called(const Checker *c, const Expr *expr);
static void check_written(Checker *c, Expr *arg, const char *doing);
static bool is_fresh(const Expr *value);
static bool through_read_only(const Expr *expr);

/* The bitfield a checked field expression names, or NULL. */
static const Field *bitfield_of(const Expr *expr) {
    if (expr->kind != EXPR_FIELD || expr->as.field.index < 0) return NULL;
    TypeKind base = expr->as.field.base->type;
    if (expr->as.field.through_pointer) base = type_target(base);
    const TypeInfo *info = type_info(base);
    if (!info || (size_t)expr->as.field.index >= info->field_count) return NULL;
    const Field *field = &info->fields[expr->as.field.index];
    return field->bits ? field : NULL;
}
static void report_copy(Checker *c, SrcLoc loc, TypeKind type, const char *instead);

/* If expr is the name of a type, possibly one that a module exports,
   returns the type's symbol; NULL, with nothing reported, otherwise. */
static Symbol *type_named_by(const Checker *c, const Expr *expr) {
    if (expr->kind != EXPR_IDENT) return NULL;
    Symbol *symbol;
    if (expr->as.ident.module.len) {
        const Symbol *owner = lookup(c, expr->as.ident.module);
        if (!owner || owner->kind != SYM_MODULE || !owner->part_count) return NULL;
        symbol = part_member(owner, expr->as.ident.name);
        if (symbol && !symbol->exported) return NULL;
    } else {
        symbol = lookup(c, expr->as.ident.name);
    }
    return symbol && symbol->kind == SYM_TYPE ? symbol : NULL;
}

/* Records the references made by an expression that names a type. */
static void use_type_name(Checker *c, const Expr *expr, Symbol *symbol) {
    if (expr->as.ident.module.len) {
        resolve_member(c, expr->as.ident.module, expr->as.ident.module_loc,
                       expr->as.ident.name, expr->as.ident.name_loc);
    } else {
        add_ref(c, expr->as.ident.name_loc, symbol);
    }
    symbol->used = true;
}

/* Finds the type that a name or typeof stands for, before any "*"s. */
static TypeKind resolve_base_type(Checker *c, const TypeRef *ref) {
    if (ref->of) {
        Symbol *named = type_named_by(c, ref->of);
        if (named) {
            use_type_name(c, ref->of, named);
            return named->type;
        }
        TypeKind type = check_operand(c, ref->of);
        if (type_is_container(type)) {
            error(c, ref->of->loc, "typeof needs a value, and an array is not one; use "
                                   "typeof on one of its elements");
            return TYPE_INVALID;
        }
        return type;
    }
    Symbol *symbol;
    if (ref->module.len) {
        symbol = resolve_member(c, ref->module, ref->module_loc, ref->name, ref->name_loc);
        if (!symbol) return TYPE_INVALID;
    } else {
        symbol = lookup(c, ref->name);
        if (!symbol) {
            error(c, ref->name_loc, "unknown type '%.*s'", NAME(ref->name));
            return TYPE_INVALID;
        }
        add_ref(c, ref->name_loc, symbol);
    }
    if (symbol->kind != SYM_TYPE) {
        error(c, ref->name_loc, "'%.*s' is a %s, not a type", NAME(ref->name),
              symbol_what(symbol));
        return TYPE_INVALID;
    }
    symbol->used = true;
    return symbol->type;
}

/* Finds the type that a type written with a name or typeof stands for.
   Returns TYPE_INVALID, with the problem reported, if there is none. */
static TypeKind signature_type(Checker *c, const FuncSig *sig);

static TypeKind resolve_type(Checker *c, const TypeRef *ref) {
    if (ref->sig) return signature_type(c, ref->sig);
    TypeKind type = resolve_base_type(c, ref);
    for (int i = 0; i < ref->depth && type != TYPE_INVALID; i++) {
        type = i == 0 && ref->read_only ? type_read_only_pointer_to(type) : type_pointer_to(type);
    }
    return type;
}

/* The type of something declared with a type that may need resolving. */
static TypeKind declared_type(Checker *c, TypeKind type, const TypeRef *ref, SrcLoc loc,
                              Str name) {
    if (!ref) return type;
    type = resolve_type(c, ref);
    if (type == TYPE_VOID) {
        error(c, loc, "'%.*s' cannot have type 'void'", NAME(name));
        return TYPE_INVALID;
    }
    return type;
}

/* Follows the ".field" steps of an assignment or read target, starting from
   a value of the given type. Returns the type of the place reached, or
   TYPE_INVALID with the problem reported. */
static TypeKind check_place_fields(Checker *c, TypeKind type, PlaceField *fields,
                                   size_t count) {
    for (size_t i = 0; i < count && type != TYPE_INVALID; i++) {
        const TypeInfo *info = type_info(type);
        if (!type_is_struct(type)) {
            error(c, fields[i].loc, "type '%s' has no fields", type_name(type));
            return TYPE_INVALID;
        }
        fields[i].index = type_field_index(info, fields[i].name);
        if (fields[i].index < 0) {
            error(c, fields[i].loc, "struct '%s' has no field '%.*s'", info->name,
                  NAME(fields[i].name));
            return TYPE_INVALID;
        }
        if (info->fields[fields[i].index].bits) {
            error(c, fields[i].loc, "'%.*s' is a bitfield, which cannot be read into; read "
                                    "into a variable, then assign it",
                  NAME(fields[i].name));
            return TYPE_INVALID;
        }
        type = info->fields[fields[i].index].type;
    }
    return type;
}



static TypeKind check_field(Checker *c, Expr *expr) {
    TypeKind base = check_expr(c, expr->as.field.base, TYPE_INVALID);
    if (base == TYPE_INVALID) return TYPE_INVALID;
    /* A pointer to a struct gives its fields with "." as well as "->". */
    if (type_is_struct(type_target(base))) {
        expr->as.field.through_pointer = true;
        base = type_target(base);
    } else if (expr->as.field.arrow) {
        error(c, expr->as.field.name_loc,
              type_is_struct(base) ? "'->' needs a pointer to a struct, found '%s'; use '.'"
                                   : "'->' needs a pointer to a struct, found '%s'",
              type_name(base));
        return TYPE_INVALID;
    }
    const TypeInfo *info = type_info(base);
    if (!type_is_struct(base)) {
        error(c, expr->as.field.name_loc, "type '%s' has no fields", type_name(base));
        return TYPE_INVALID;
    }
    expr->as.field.index = type_field_index(info, expr->as.field.name);
    if (expr->as.field.index < 0) {
        error(c, expr->as.field.name_loc, "struct '%s' has no field '%.*s'", info->name,
              NAME(expr->as.field.name));
        return TYPE_INVALID;
    }
    return info->fields[expr->as.field.index].type;
}

/* Checks a "{ ... }" value against the type its context asks for: a struct,
   whose fields it gives, or an array or a growable array, whose elements
   it lists. */
static TypeKind check_init(Checker *c, Expr *expr, TypeKind hint) {
    ContainerKind kind = type_container(hint);
    if (kind == CONTAINER_ARRAY || kind == CONTAINER_LIST) {
        TypeKind elem = type_elem(hint);
        size_t length = type_length(hint);
        for (size_t i = 0; i < expr->as.init.count; i++) {
            InitItem *item = &expr->as.init.items[i];
            if (item->field.len) {
                error(c, item->field_loc, "an array's elements are listed without names");
            }
            if (kind == CONTAINER_ARRAY && i == length) {
                error(c, item->value->loc, "too many elements: the array has length %zu",
                      length);
            }
            TypeKind found = check_value(c, item->value, elem);
            if (elem != TYPE_INVALID && found != TYPE_INVALID && found != elem) {
                error(c, item->value->loc, "an element of this array must be '%s', found '%s'",
                      type_name(elem), type_name(found));
            }
        }
        return hint;
    }
    const TypeInfo *info = type_info(hint);
    if (!type_is_struct(hint)) {
        if (hint == TYPE_INVALID) {
            error(c, expr->loc, "this '{ ... }' has nothing to tell it which struct it "
                                "is; write the type first, as in '(Type){ ... }'");
        } else {
            error(c, expr->loc, "type '%s' cannot be written as '{ ... }'", type_name(hint));
        }
        for (size_t i = 0; i < expr->as.init.count; i++) {
            check_operand(c, expr->as.init.items[i].value);
        }
        return TYPE_INVALID;
    }
    if (info->is_union && expr->as.init.count > 1) {
        error(c, expr->as.init.items[1].value->loc,
              "a union holds one of its fields at a time, so it is given one value");
    }
    size_t position = 0;
    for (size_t i = 0; i < expr->as.init.count; i++) {
        InitItem *item = &expr->as.init.items[i];
        if (item->field.len) {
            item->index = type_field_index(info, item->field);
            if (item->index < 0) {
                error(c, item->field_loc, "struct '%s' has no field '%.*s'", info->name,
                      NAME(item->field));
            } else {
                position = (size_t)item->index + 1;
            }
        } else if (position < info->field_count) {
            item->index = (int)position++;
        } else {
            error(c, item->value->loc, "too many values: struct '%s' has %zu field%s",
                  info->name, info->field_count, info->field_count == 1 ? "" : "s");
        }
        if (item->index < 0) {
            check_operand(c, item->value);
            continue;
        }
        TypeKind expected = info->fields[item->index].type;
        if (type_container(expected) == CONTAINER_MAP) {
            error(c, item->value->loc, "field '%.*s' is a map, which starts empty; add to it "
                                       "once the struct is made",
                  NAME(info->fields[item->index].name));
            continue;
        }
        for (size_t j = 0; j < i; j++) {
            if (expr->as.init.items[j].index == item->index) {
                error(c, item->value->loc, "field '%.*s' is given a value twice",
                      NAME(info->fields[item->index].name));
                break;
            }
        }
        TypeKind found = check_value(c, item->value, expected);
        if (expected != TYPE_INVALID && found != TYPE_INVALID && found != expected) {
            error(c, item->value->loc, "field '%.*s' is '%s', found '%s'",
                  NAME(info->fields[item->index].name), type_name(expected),
                  type_name(found));
        }
    }
    return hint;
}

/* An expression as it is written, quoted, for a message: "'xs'" or
   "'rows[i]'", or "this" for one that is long or spans lines. */
static const char *quoted(Checker *c, const Expr *expr) {
    Str text = {0};
    if (expr->kind == EXPR_IDENT && !expr->as.ident.module.len) text = expr->as.ident.name;
    if (expr->kind == EXPR_FIELD) text = expr->as.field.name;
    if (expr->kind == EXPR_INDEX && expr->loc.offset + expr->loc.len <= c->unit->src.len) {
        text = (Str){c->unit->src.data + expr->loc.offset, expr->loc.len};
    }
    if (!text.len || text.len > 40 || memchr(text.data, '\n', text.len)) return "this";
    char *quoted_text = arena_alloc(c->arena, text.len + 3);
    snprintf(quoted_text, text.len + 3, "'%.*s'", NAME(text));
    return quoted_text;
}

/* Checks an expression where a whole array or map may stand, as in
   len(xs) or append(xs, v), and returns its type: a container if it is
   one. */
static TypeKind check_operand(Checker *c, Expr *expr) {
    bool want_array = c->want_array;
    c->want_array = true;
    TypeKind type = check_expr(c, expr, TYPE_INVALID);
    c->want_array = want_array;
    return type;
}

/* Checks an expression used as an index. A negative index counts back from
   the end, so an array of length n accepts -n to n - 1. For a fixed array a
   literal index is range-checked here; everything else is checked when the
   program runs. */
/* What an error about a 'char' used as a number adds. */
static const char *char_hint(bool involved) {
    return involved ? "; a 'char' is a character, not a number: 'int(c)' is its code, and "
                      "'char(n)' is the character with code n"
                    : "";
}

static bool const_len(const Expr *operand, size_t *length);
static bool eval_const_int(const Expr *expr, long long *out);

/* What an index is applied to, for check_index_value. */
typedef struct {
    bool pointer;       /* "p[n]", which may reach before p as in C */
    bool length_known;  /* a fixed array, or a string whose length is constant */
    size_t length;
    const char *runtime_length; /* why the length is not known, for a message */
    const char *what;           /* "array" or "string", for a message */
    Str name;                   /* what is indexed, if it is a name */
} Indexed;

/* Checks an index. One whose value is known at compile time is checked
   against a length known then; a negative one counts back from the end and
   becomes the position it names, so that no negative index is left for the
   program to deal with. A negative index whose length is only known when the
   program runs is an error. Everything else is checked when the program runs. */
static void check_index_value(Checker *c, Expr *index, Indexed of) {
    TypeKind type = check_expr(c, index, TYPE_INT);
    if (type == TYPE_INVALID) return;
    if (!type_is_integer(type) || type == TYPE_I128 || type == TYPE_U128) {
        error(c, index->loc, "an index must be an integer of at most 64 bits, found '%s'%s",
              type_name(type), char_hint(type == TYPE_CHAR));
        return;
    }
    long long value;
    if (of.pointer || !eval_const_int(index, &value)) return;
    if (type_is_signed(type) && value < 0) {
        unsigned long long back = 0ULL - (unsigned long long)value;
        if (!of.length_known && of.name.len) {
            error(c, index->loc,
                  "index %lld counts back from the end, which needs a length known at "
                  "compile time, and %s; write '%.*s[len(%.*s) - %llu]'",
                  value, of.runtime_length, NAME(of.name), NAME(of.name), back);
        } else if (!of.length_known) {
            error(c, index->loc,
                  "index %lld counts back from the end, which needs a length known at "
                  "compile time, and %s; subtract from 'len' instead",
                  value, of.runtime_length);
        } else if (back > of.length) {
            error(c, index->loc, "index %lld is out of range for %s of length %zu", value,
                  of.what, of.length);
        } else {
            index->kind = EXPR_INT_LIT;
            index->as.int_lit = of.length - back;
        }
    } else if (of.length_known && (unsigned long long)value >= of.length) {
        error(c, index->loc, "index %llu is out of range for %s of length %zu",
              (unsigned long long)value, of.what, of.length);
    }
}

static TypeKind check_index(Checker *c, Expr *expr) {
    Expr *base = expr->as.index.base;
    TypeKind type = check_operand(c, base);
    ContainerKind kind = type_container(type);
    if (kind == CONTAINER_MAP) {
        Expr *key = expr->as.index.index;
        TypeKind want = type_key(type);
        TypeKind found = check_expr(c, key, want);
        if (found != TYPE_INVALID && found != want) {
            error(c, key->loc, "a key of map %s must be '%s', found '%s'", quoted(c, base),
                  type_name(want), type_name(found));
        }
        return type_elem(type);
    }
    TypeKind result = TYPE_INVALID;
    if (kind != CONTAINER_NONE) {
        result = type_elem(type);
    } else if (type == TYPE_STRING) {
        result = TYPE_CHAR;
    } else if (type_is_pointer(type)) {
        /* "p[n]" is the element n places on from the one p points to. */
        result = type_target(type);
        if (result == TYPE_VOID) {
            error(c, base->loc, "a 'void *' points to nothing of a known type; convert it "
                                "first, as in '(int *)p'");
            result = TYPE_INVALID;
        }
    } else if (type != TYPE_INVALID) {
        error(c, base->loc, "cannot index a value of type '%s'; only arrays, "
                            "strings and pointers have elements",
              type_name(type));
    }
    Indexed of = {.what = "an array"};
    if (base->kind == EXPR_IDENT && !base->as.ident.module.len) of.name = base->as.ident.name;
    if (kind == CONTAINER_ARRAY) {
        of.length_known = true;
        of.length = type_length(type);
    } else if (kind != CONTAINER_NONE) {
        of.runtime_length = kind == CONTAINER_LIST
                                ? "a growable array's length is known only when the program runs"
                                : "an array parameter's length is known only when the program runs";
    } else if (type == TYPE_STRING) {
        of.what = "a string";
        of.length_known = const_len(base, &of.length);
        of.runtime_length = "this string's length is known only when the program runs";
    } else {
        of.pointer = true;
    }
    check_index_value(c, expr->as.index.index, of);
    return result;
}

/* Checks one end of a slice: an integer, and not one known to be negative. */
static void check_slice_end(Checker *c, Expr *end) {
    if (!end) return;
    TypeKind type = check_expr(c, end, TYPE_INT);
    long long value;
    if (type == TYPE_INVALID) return;
    if (!type_is_integer(type) || type == TYPE_I128 || type == TYPE_U128) {
        error(c, end->loc, "a slice's ends are integers of at most 64 bits, found '%s'%s",
              type_name(type), char_hint(type == TYPE_CHAR));
    } else if (type_is_signed(type) && eval_const_int(end, &value) && value < 0) {
        error(c, end->loc, "a slice's ends are positions from 0 to the string's length; "
                           "count from the end with 'len(s) - %lld'",
              -value);
    }
}

/* "s[from:to]": a view of part of a string, which must be one. */
static TypeKind check_slice(Checker *c, Expr *expr) {
    Expr *base = expr->as.slice.base;
    TypeKind type = check_operand(c, base);
    if (any_array(type)) {
        Str name = base->kind == EXPR_IDENT ? base->as.ident.name : (Str){"chars", 5};
        error(c, base->loc, "only a string can be sliced; 'string(%.*s)' is a view of a "
                            "growable array of char",
              NAME(name));
    } else if (type_is_container(type)) {
        error(c, base->loc, "only a string can be sliced, found '%s'", type_name(type));
    } else if (type != TYPE_STRING && type != TYPE_INVALID) {
        error(c, base->loc, "only a string can be sliced, found '%s'", type_name(type));
    }
    check_slice_end(c, expr->as.slice.from);
    check_slice_end(c, expr->as.slice.to);
    return TYPE_STRING;
}

/* True for a call to a built-in function of the given name, which is used
   unless the program declares the name itself. */
static bool is_builtin(const Checker *c, const Expr *expr, const char *name) {
    return !expr->as.call.module.len && str_eq_cstr(expr->as.call.name, name) &&
           !lookup(c, expr->as.call.name);
}

static bool is_char_pointer(TypeKind type) {
    return type_target(type) == TYPE_CHAR;
}

/* The built-in cstring(s): a copy of a string's bytes with a '\0' after
   them, as a "char *". It becomes a conversion of the string. */
static TypeKind check_cstring(Checker *c, Expr *expr) {
    size_t count = expr->as.call.count;
    Expr *operand = count ? expr->as.call.args[0] : NULL;
    for (size_t i = 0; i < count; i++) {
        TypeKind type = check_expr(c, expr->as.call.args[i], TYPE_INVALID);
        if (count == 1 && type != TYPE_STRING && type != TYPE_INVALID) {
            error(c, operand->loc, "'cstring' needs a string, found '%s'", type_name(type));
        }
    }
    TypeKind to = type_pointer_to(TYPE_CHAR);
    if (count != 1) {
        error(c, expr->loc, "'cstring' takes 1 argument, but %zu were given", count);
        return to;
    }
    expr->kind = EXPR_CAST;
    expr->as.cast.to = to;
    expr->as.cast.to_ref = NULL;
    expr->as.cast.operand = operand;
    return to;
}

/* The built-in len(x): the number of elements of an array or bytes of a
   string. */

static TypeKind check_len(Checker *c, Expr *expr) {
    if (expr->as.call.count != 1) {
        error(c, expr->loc, "'len' takes 1 argument, but %zu were given",
              expr->as.call.count);
        for (size_t i = 0; i < expr->as.call.count; i++) check_operand(c, expr->as.call.args[i]);
        return TYPE_INT;
    }
    Expr *operand = expr->as.call.args[0];
    TypeKind type = check_operand(c, operand);
    if (!type_is_container(type) && type != TYPE_STRING && type != TYPE_INVALID) {
        error(c, operand->loc, "'len' needs an array, a map or a string, found '%s'",
              type_name(type));
    }
    expr->kind = EXPR_LEN;
    expr->as.len.operand = operand;
    return TYPE_INT;
}

static bool is_arithmetic(Op op) {
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV ||
           op == OP_MOD;
}

static bool is_bitwise(Op op) {
    return op == OP_BIT_AND || op == OP_BIT_OR || op == OP_BIT_XOR;
}

bool op_is_shift(Op op) {
    return op == OP_SHL || op == OP_SHR;
}

/* True for the binary operators that produce a number from numbers. */
static bool is_numeric_op(Op op) {
    return is_arithmetic(op) || is_bitwise(op) || op_is_shift(op);
}

/* True for an expression built only from number literals and arithmetic,
   which has no type of its own and adopts the one its context needs. */
static bool is_untyped(const Expr *expr) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_SIZEOF:
        return true;
    case EXPR_UNARY:
        return (expr->as.unary.op == OP_NEG || expr->as.unary.op == OP_BIT_NOT) &&
               is_untyped(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_numeric_op(expr->as.binary.op) &&
               is_untyped(expr->as.binary.left) && is_untyped(expr->as.binary.right);
    case EXPR_TERNARY:
        return is_untyped(expr->as.ternary.then) &&
               is_untyped(expr->as.ternary.otherwise);
    default:
        return false;
    }
}

static bool has_float_literal(const Expr *expr) {
    switch (expr->kind) {
    case EXPR_FLOAT_LIT:
        return true;
    case EXPR_UNARY:
        return has_float_literal(expr->as.unary.operand);
    case EXPR_BINARY:
        return has_float_literal(expr->as.binary.left) ||
               has_float_literal(expr->as.binary.right);
    case EXPR_TERNARY:
        return has_float_literal(expr->as.ternary.then) ||
               has_float_literal(expr->as.ternary.otherwise);
    default:
        return false;
    }
}

/* Checks the two operands of a binary operator so that an untyped side
   adopts the type of the typed one, whichever side it is on. */
static void check_operands(Checker *c, Expr *left, Expr *right, TypeKind hint) {
    /* NULL takes the pointer type of the other side. */
    if (left->kind == EXPR_NULL && right->kind != EXPR_NULL) {
        check_expr(c, left, check_expr(c, right, hint));
        return;
    }
    if (right->kind == EXPR_NULL) {
        check_expr(c, right, check_expr(c, left, hint));
        return;
    }
    if (is_untyped(left) && is_untyped(right)) {
        if (!type_is_numeric(hint)) {
            hint = (has_float_literal(left) || has_float_literal(right)) ? TYPE_FLOAT
                                                                         : TYPE_INT;
        }
        check_expr(c, left, hint);
        check_expr(c, right, hint);
    } else if (is_untyped(left)) {
        check_expr(c, left, check_expr(c, right, hint));
    } else {
        TypeKind type = check_expr(c, left, hint);
        check_expr(c, right, is_untyped(right) ? type : hint);
    }
}

static TypeKind check_int_literal(Checker *c, Expr *expr, TypeKind hint, bool negated) {
    TypeKind type = type_is_numeric(hint) ? hint : TYPE_INT;
    if (!type_is_integer(type)) return type;
    U128 limit = negated ? type_int_min_magnitude(type) : type_int_max(type);
    if (expr->as.int_lit > limit) {
        error(c, expr->loc, "%s does not fit in '%s'",
              negated ? "negative literal" : "integer literal", type_name(type));
    }
    return type;
}

static TypeKind check_place(Checker *c, Expr *expr, bool assigning);

/* True for a type that can be tested for truth: bool, and a pointer, which
   is true unless it is NULL. */
static bool is_truth(TypeKind type) {
    return type == TYPE_BOOL || type_is_pointer(type) || type_is_function(type);
}

static TypeKind check_unary(Checker *c, Expr *expr, TypeKind hint) {
    Expr *operand = expr->as.unary.operand;
    if (expr->as.unary.op == OP_ADDR) {
        const Symbol *named = operand->kind == EXPR_IDENT && !operand->as.ident.module.len
                                  ? lookup(c, operand->as.ident.name)
                                  : NULL;
        if (named && named->kind == SYM_FUNC) return check_expr(c, operand, TYPE_INVALID);
        TypeKind type = check_place(c, operand, false);
        if (type == TYPE_INVALID) return TYPE_INVALID;
        if (bitfield_of(operand)) {
            error(c, operand->loc, "'%.*s' is a bitfield, which has no address of its own",
                  NAME(operand->as.field.name));
            return TYPE_INVALID;
        }
        const Expr *mover = moving_part(operand);
        if (mover && type_container(mover->type) == CONTAINER_MAP) {
            error(c, operand->loc, "a value in map %s moves when the map grows, so it cannot "
                                   "be pointed to; keep its key instead",
                  quoted(c, mover));
            return TYPE_INVALID;
        }
        if (mover) {
            error(c, operand->loc, "an element of growable array %s moves when the array "
                                   "grows, so it cannot be pointed to; keep its index instead",
                  quoted(c, mover));
            return TYPE_INVALID;
        }
        const Expr *root = operand;
        while ((root->kind == EXPR_FIELD && !root->as.field.through_pointer) ||
               root->kind == EXPR_INDEX) {
            root = root->kind == EXPR_FIELD ? root->as.field.base : root->as.index.base;
        }
        if (root->kind == EXPR_IDENT && root->as.ident.symbol) {
            ((Symbol *)root->as.ident.symbol)->address_taken = true;
        }
        return through_read_only(operand) ? type_read_only_pointer_to(type)
                                          : type_pointer_to(type);
    }
    if (expr->as.unary.op == OP_DEREF) {
        TypeKind type = check_expr(c, operand, TYPE_INVALID);
        if (type == TYPE_INVALID) return TYPE_INVALID;
        /* "*f" of a pointer to a function is the function, which C reads as
           the pointer again. */
        if (type_is_function(type)) return type;
        if (!type_is_pointer(type)) {
            error(c, operand->loc, "operator '*' needs a pointer, found '%s'",
                  type_name(type));
            return TYPE_INVALID;
        }
        if (type_target(type) == TYPE_VOID) {
            error(c, operand->loc, "a 'void *' points to nothing of a known type; convert it "
                                   "first, as in '*(int *)p'");
            return TYPE_INVALID;
        }
        return type_target(type);
    }
    if (expr->as.unary.op == OP_NOT) {
        TypeKind type = check_expr(c, operand, TYPE_INVALID);
        if (!is_truth(type) && type != TYPE_INVALID) {
            error(c, operand->loc, "operator '!' needs a 'bool' or a pointer, found '%s'",
                  type_name(type));
        }
        return TYPE_BOOL;
    }
    if (expr->as.unary.op == OP_BIT_NOT) {
        TypeKind type = check_expr(c, operand, hint);
        if (type != TYPE_INVALID && !type_is_integer(type)) {
            error(c, operand->loc, "operator '~' needs an integer operand, found '%s'",
                  type_name(type));
            return TYPE_INVALID;
        }
        return type;
    }
    /* A negated literal is range-checked as one value, so that the most
       negative number of a type can be written. */
    if (operand->kind == EXPR_INT_LIT) {
        operand->type = check_int_literal(c, operand, hint, true);
        return operand->type;
    }
    TypeKind type = check_expr(c, operand, hint);
    if (type != TYPE_INVALID && !type_is_numeric(type)) {
        error(c, operand->loc, "operator '-' needs a numeric operand, found '%s'",
              type_name(type));
        return TYPE_INVALID;
    }
    return type;
}

/* A shift amount is an integer of any type narrow enough to be passed to
   the shift helpers. */
static void check_shift_amount(Checker *c, Expr *amount) {
    TypeKind type = check_expr(c, amount, TYPE_INVALID);
    if (type == TYPE_INVALID) return;
    if (!type_is_integer(type) || type == TYPE_I128 || type == TYPE_U128) {
        error(c, amount->loc,
              "a shift amount must be an integer of at most 64 bits, found '%s'",
              type_name(type));
    }
}

/* True for a type that can count the elements a pointer moves by. */
static bool is_offset(TypeKind type) {
    return type_is_integer(type) && type != TYPE_I128 && type != TYPE_U128;
}

static bool converts_to(TypeKind from, TypeKind to);

static TypeKind check_binary(Checker *c, Expr *expr, TypeKind hint) {
    Op op = expr->as.binary.op;
    Expr *left = expr->as.binary.left;
    Expr *right = expr->as.binary.right;

    /* left, right or fixed on either side of "+" joins text, as it does in
       cout; the other side is then written as text too. */
    bool left_format = left->kind == EXPR_CALL && is_format(builtin_called(c, left));
    bool right_format = right->kind == EXPR_CALL && is_format(builtin_called(c, right));
    if (op == OP_ADD && (left_format || right_format)) {
        check_written(c, left, "join");
        check_written(c, right, "join");
        return TYPE_STRING;
    }

    if (op == OP_AND || op == OP_OR) {
        Expr *sides[2] = {left, right};
        for (int i = 0; i < 2; i++) {
            TypeKind type = check_expr(c, sides[i], TYPE_INVALID);
            if (!is_truth(type) && type != TYPE_INVALID) {
                error(c, sides[i]->loc,
                      "operator '%s' needs 'bool' or pointer operands, found '%s'",
                      op_text(op), type_name(type));
            }
        }
        return TYPE_BOOL;
    }

    if (op_is_shift(op)) {
        /* The result has the type of the value being shifted. The amount is
           checked on its own and may be of a different integer type. */
        TypeKind type = check_expr(c, left, hint);
        check_shift_amount(c, right);
        if (type != TYPE_INVALID && !type_is_integer(type)) {
            error(c, left->loc, "operator '%s' is not defined for '%s'", op_text(op),
                  type_name(type));
            return TYPE_INVALID;
        }
        return type;
    }

    bool arithmetic = is_arithmetic(op) || is_bitwise(op);
    check_operands(c, left, right, arithmetic ? hint : TYPE_INVALID);
    TypeKind result = arithmetic ? left->type : TYPE_BOOL;
    TypeKind type = left->type;

    if (left->type == TYPE_INVALID || right->type == TYPE_INVALID) {
        return arithmetic ? TYPE_INVALID : TYPE_BOOL;
    }
    /* "+" with a string on either side joins the two as text, the other
       written as cout would write it. */
    if (op == OP_ADD && (left->type == TYPE_STRING || right->type == TYPE_STRING)) {
        for (int side = 0; side < 2; side++) {
            const Expr *part = side ? right : left;
            if (part->type == TYPE_VOID) {
                error(c, part->loc, "cannot join a 'void' value to a string");
            }
        }
        return TYPE_STRING;
    }
    bool left_pointer = type_is_pointer(left->type);
    bool right_pointer = type_is_pointer(right->type);
    if (!arithmetic && left->decayed && right->decayed) {
        error(c, expr->loc, "operator '%s' would compare where the two arrays are, not "
                            "what they hold; compare their elements in a loop",
              op_text(op));
        return TYPE_BOOL;
    }
    if ((left_pointer || right_pointer) && (op == OP_ADD || op == OP_SUB)) {
        /* A pointer moves by a count of elements, and two pointers of one
           type are that many elements apart. */
        if ((left_pointer && type_target(left->type) == TYPE_VOID) ||
            (right_pointer && type_target(right->type) == TYPE_VOID)) {
            error(c, expr->loc, "a 'void *' points to nothing of a known size, so it cannot "
                                "be moved; convert it first, as in '(u8 *)p'");
            return TYPE_INVALID;
        }
        if (left_pointer && is_offset(right->type)) return left->type;
        if (right_pointer && op == OP_ADD && is_offset(left->type)) return right->type;
        if (op == OP_SUB && left->type == right->type) return TYPE_INT;
        error(c, expr->loc, "operator '%s' is not defined for '%s' and '%s'", op_text(op),
              type_name(left->type), type_name(right->type));
        return TYPE_INVALID;
    }
    /* Two pointers compare when one converts to the other's type. */
    if ((op == OP_EQ || op == OP_NE) &&
        (converts_to(left->type, right->type) || converts_to(right->type, left->type))) {
        return TYPE_BOOL;
    }
    if (left->type != right->type) {
        error(c, expr->loc, "mismatched types '%s' and '%s' for operator '%s'%s",
              type_name(left->type), type_name(right->type), op_text(op),
              char_hint(arithmetic && (left->type == TYPE_CHAR || right->type == TYPE_CHAR)));
        return arithmetic ? TYPE_INVALID : TYPE_BOOL;
    }

    bool defined;
    if (op == OP_EQ || op == OP_NE) {
        defined = type != TYPE_VOID && !type_is_struct(type);
    } else if (op == OP_MOD || is_bitwise(op)) {
        defined = type_is_integer(type);
    } else if (arithmetic) {
        defined = type_is_numeric(type);
    } else {
        defined = type_is_numeric(type) || type == TYPE_CHAR || type_is_enum(type) ||
                  type_is_pointer(type) || type == TYPE_STRING;
    }
    if (!defined) {
        error(c, expr->loc, "operator '%s' is not defined for '%s'%s", op_text(op),
              type_name(type), char_hint(type == TYPE_CHAR));
        return arithmetic ? TYPE_INVALID : TYPE_BOOL;
    }
    return result;
}

static TypeKind check_ternary(Checker *c, Expr *expr, TypeKind hint) {
    Expr *cond = expr->as.ternary.cond;
    Expr *then = expr->as.ternary.then;
    Expr *otherwise = expr->as.ternary.otherwise;

    TypeKind cond_type = check_expr(c, cond, TYPE_INVALID);
    if (!is_truth(cond_type) && cond_type != TYPE_INVALID) {
        error(c, cond->loc, "the condition of '?:' must be 'bool' or a pointer, found '%s'",
              type_name(cond_type));
    }
    check_operands(c, then, otherwise, hint);
    if (then->type == TYPE_INVALID || otherwise->type == TYPE_INVALID) return TYPE_INVALID;
    if (then->type != otherwise->type) {
        error(c, expr->loc, "the two results of '?:' have different types: '%s' and '%s'",
              type_name(then->type), type_name(otherwise->type));
        return TYPE_INVALID;
    }
    if (then->type == TYPE_VOID) {
        error(c, expr->loc, "the results of '?:' cannot be 'void'");
        return TYPE_INVALID;
    }
    return then->type;
}

static TypeKind check_cast(Checker *c, Expr *expr);

/* The type of a pointer to a function: what it returns, and takes. */
static TypeKind function_type(Checker *c, const Stmt *func) {
    size_t count = func->as.func.param_count;
    TypeKind *params = arena_alloc(c->arena, (count ? count : 1) * sizeof *params);
    for (size_t i = 0; i < count; i++) params[i] = func->as.func.params[i].type;
    return type_function_of(func->as.func.ret, params, count);
}

static TypeKind check_args(Checker *c, Expr *expr, Str name, const Param *params,
                           size_t param_count, bool known, TypeKind ret);

/* Checks the values a call gives after the parameters of a function that
   ends with "...", which C takes as they are: numbers, characters, enums
   and pointers. A string literal there is C's kind of string. The checked
   values are then left out of the call's own count. */
static void check_variadic_args(Checker *c, Expr *expr, const Stmt *func) {
    size_t fixed = func->as.func.param_count;
    if (expr->as.call.count < fixed) return; /* check_args reports it */
    for (size_t i = fixed; i < expr->as.call.count; i++) {
        Expr *arg = expr->as.call.args[i];
        TypeKind type = check_expr(c, arg, type_read_only_pointer_to(TYPE_CHAR));
        if (type == TYPE_STRING) {
            error(c, arg->loc, "a string cannot go through '...', which takes C's strings; "
                               "pass 'cstring(...)'");
        } else if (type == TYPE_VOID || type_is_struct(type) || type_is_container(type)) {
            error(c, arg->loc, "a '%s' cannot go through '...', which takes numbers, "
                               "characters and pointers",
                  type_name(type));
        }
    }
}

/* A call through a pointer to a function, which callee gives. */
static TypeKind check_pointer_call(Checker *c, Expr *expr, Expr *callee) {
    expr->as.call.callee = callee;
    TypeKind type = check_expr(c, callee, TYPE_INVALID);
    const TypeInfo *info = type_info(type);
    if (!type_is_function(type)) {
        if (type != TYPE_INVALID) {
            error(c, callee->loc, "%s is a '%s', not a function", quoted(c, callee),
                  type_name(type));
        }
        check_args(c, expr, (Str){0}, NULL, 0, false, TYPE_INVALID);
        return TYPE_INVALID;
    }
    Param *params = arena_alloc(c->arena, (info->param_count ? info->param_count : 1) *
                                              sizeof *params);
    memset(params, 0, (info->param_count ? info->param_count : 1) * sizeof *params);
    for (size_t i = 0; i < info->param_count; i++) params[i].type = info->params[i];
    Str name = callee->kind == EXPR_IDENT ? callee->as.ident.name
               : callee->kind == EXPR_FIELD ? callee->as.field.name
                                            : (Str){"the function", 12};
    return check_args(c, expr, name, params, info->param_count, true, info->target);
}

static TypeKind check_call(Checker *c, Expr *expr) {
    Str name = expr->as.call.name;
    if (expr->as.call.callee) return check_pointer_call(c, expr, expr->as.call.callee);
    /* "a.f(x)" calls a pointer to a function that a struct holds, unless a
       is a module; "f(x)" one that a variable holds. */
    const Symbol *owner = expr->as.call.module.len ? lookup(c, expr->as.call.module) : NULL;
    const Symbol *local = expr->as.call.module.len ? NULL : lookup(c, name);
    if ((owner && owner->kind != SYM_MODULE) ||
        (local && local->kind != SYM_FUNC && local->kind != SYM_TYPE &&
         type_is_function(local->type))) {
        Expr *callee = arena_alloc(c->arena, sizeof *callee);
        memset(callee, 0, sizeof *callee);
        callee->kind = EXPR_IDENT;
        callee->loc = expr->as.call.module.len ? expr->as.call.module_loc : expr->as.call.name_loc;
        callee->as.ident.name = name;
        callee->as.ident.name_loc = expr->as.call.name_loc;
        callee->as.ident.module = expr->as.call.module;
        callee->as.ident.module_loc = expr->as.call.module_loc;
        return check_pointer_call(c, expr, callee);
    }
    Symbol *symbol = expr->as.call.module.len
                         ? resolve_member(c, expr->as.call.module, expr->as.call.module_loc,
                                          name, expr->as.call.name_loc)
                         : resolve(c, name, expr->as.call.name_loc);
    const Stmt *func = NULL;
    if (symbol && symbol->kind == SYM_TYPE && expr->as.call.count == 1) {
        /* "Name(value)" converts to a declared type, as "int(value)" does
           to a built-in one. */
        Expr *operand = expr->as.call.args[0];
        symbol->used = true;
        expr->kind = EXPR_CAST;
        expr->as.cast.to = symbol->type;
        expr->as.cast.to_ref = NULL;
        expr->as.cast.operand = operand;
        return check_cast(c, expr);
    }
    if (symbol && symbol->kind == SYM_TYPE) {
        error(c, expr->loc, "a conversion to '%.*s' takes one value", NAME(name));
        symbol = NULL;
    }
    if (symbol && symbol->kind != SYM_FUNC) {
        error(c, expr->as.call.name_loc, "'%.*s' is not a function", NAME(name));
    } else if (symbol) {
        symbol->used = true;
        expr->as.call.symbol = symbol;
        func = symbol->decl;
    }

    if (func && func->as.func.variadic) check_variadic_args(c, expr, func);
    return check_args(c, expr, name, func ? func->as.func.params : NULL,
                      func ? func->as.func.param_count : 0, func != NULL,
                      func ? func->as.func.ret : TYPE_INVALID);
}

/* Checks a call's arguments against the parameters, when they are known,
   and returns what the call gives. */
static TypeKind check_args(Checker *c, Expr *expr, Str name, const Param *params,
                           size_t param_count, bool known, TypeKind ret) {
    size_t count = expr->as.call.count;
    /* The values after the parameters of a call to a function that ends
       with "..." are checked already. */
    bool variadic = known && expr->as.call.symbol && expr->as.call.symbol->decl &&
                    expr->as.call.symbol->decl->kind == STMT_FUNC &&
                    expr->as.call.symbol->decl->as.func.variadic;
    if (variadic && count >= param_count) count = param_count;
    if (variadic && count < param_count) {
        error(c, expr->loc, "'%.*s' takes at least %zu argument%s, but %zu %s given", NAME(name),
              param_count, param_count == 1 ? "" : "s", count, count == 1 ? "was" : "were");
    } else if (known && count != param_count) {
        error(c, expr->loc, "'%.*s' takes %zu argument%s, but %zu %s given", NAME(name),
              param_count, param_count == 1 ? "" : "s",
              count, count == 1 ? "was" : "were");
    }
    for (size_t i = 0; i < count; i++) {
        Expr *arg = expr->as.call.args[i];
        bool has_param = known && i < param_count;
        TypeKind expected = has_param ? params[i].type : TYPE_INVALID;
        ContainerKind wanted = type_container(expected);
        if (has_param && wanted != CONTAINER_NONE) {
            const Param *param = &params[i];
            TypeKind found = check_operand(c, arg);
            ContainerKind kind = type_container(found);
            if (found == TYPE_INVALID) continue;
            if (wanted == CONTAINER_MAP) {
                if (kind == CONTAINER_MAP && found != expected) {
                    error(c, arg->loc,
                          "argument %zu of '%.*s' must be a map from '%s' to '%s', found one "
                          "from '%s' to '%s'",
                          i + 1, NAME(name), type_name(type_key(expected)),
                          type_name(type_elem(expected)), type_name(type_key(found)),
                          type_name(type_elem(found)));
                } else if (kind != CONTAINER_MAP) {
                    error(c, arg->loc, "argument %zu of '%.*s' must be a map from '%s' to "
                                       "'%s', found '%s'",
                          i + 1, NAME(name), type_name(type_key(expected)),
                          type_name(type_elem(expected)), type_name(found));
                } else if (!report_shared(c, arg, "changed")) {
                    mark_changed(arg);
                }
                continue;
            }
            TypeKind elem = type_elem(expected);
            if (wanted == CONTAINER_LIST) {
                if (kind == CONTAINER_LIST && found != expected) {
                    error(c, arg->loc,
                          "argument %zu of '%.*s' must be a growable array of '%s', found one "
                          "of '%s'",
                          i + 1, NAME(name), type_name(elem), type_name(type_elem(found)));
                } else if (kind == CONTAINER_ARRAY || kind == CONTAINER_SLICE) {
                    error(c, arg->loc,
                          "argument %zu of '%.*s' must be a growable array of '%s', and %s %s",
                          i + 1, NAME(name), type_name(elem), quoted(c, arg),
                          kind == CONTAINER_SLICE ? "is a parameter's view of an array, which "
                                                    "cannot grow"
                                                  : "has a fixed length");
                } else if (kind != CONTAINER_LIST) {
                    error(c, arg->loc, "argument %zu of '%.*s' must be a growable array of "
                                       "'%s', found '%s'",
                          i + 1, NAME(name), type_name(elem), type_name(found));
                } else if (!report_shared(c, arg, "changed")) {
                    mark_changed(arg);
                }
                continue;
            }
            /* A view: of any array whose elements are of its type. */
            const Symbol *root = place_root(arg);
            if (any_array(found) && type_elem(found) != elem) {
                error(c, arg->loc,
                      "argument %zu of '%.*s' must be an array of '%s', found an array "
                      "of '%s'",
                      i + 1, NAME(name), type_name(elem), type_name(type_elem(found)));
            } else if (!any_array(found)) {
                error(c, arg->loc,
                      "argument %zu of '%.*s' must be an array of '%s', found '%s'%s", i + 1,
                      NAME(name), type_name(elem), type_name(found),
                      type_is_pointer(found) ? "; a pointer does not know how many elements "
                                               "it points to, and an array does"
                                             : "");
            } else if (through_read_only(arg) && !param->is_const) {
                error(c, arg->loc, "this array is reached through a 'const' pointer, so it can "
                                   "only be given to an array parameter declared 'const'");
            } else if (root && (root->kind == SYM_CONST || root->read_only) &&
                       !param->is_const) {
                error(c, arg->loc,
                      "%s '%.*s' can only be given to an array parameter declared 'const', "
                      "which cannot change it",
                      root->kind == SYM_CONST ? "constant array" : "read-only array",
                      NAME(root->name));
            } else if (!param->is_const && moving_part(arg) &&
                       type_container(moving_part(arg)->type) == CONTAINER_MAP) {
                mark_changed(arg);
            }
            continue;
        }
        TypeKind type = check_expr(c, arg, expected);
        if (expected != TYPE_INVALID && type != TYPE_INVALID && type != expected) {
            error(c, arg->loc, "argument %zu of '%.*s' must be '%s', found '%s'", i + 1,
                  NAME(name), type_name(expected), type_name(type));
        }
    }
    return known ? ret : TYPE_INVALID;
}

/* The built-in functions on growable arrays, by name. */
static const struct {
    const char *name;
    Builtin builtin;
} builtin_names[] = {
    {"append", BUILTIN_APPEND}, {"pop", BUILTIN_POP},       {"clear", BUILTIN_CLEAR},
    {"reserve", BUILTIN_RESERVE}, {"delete", BUILTIN_DELETE}, {"clone", BUILTIN_CLONE},
    {"has", BUILTIN_HAS},         {"remove", BUILTIN_REMOVE},     {"left", BUILTIN_LEFT},
    {"right", BUILTIN_RIGHT},     {"fixed", BUILTIN_FIXED},       {"exit", BUILTIN_EXIT},
    {"sort", BUILTIN_SORT},       {"reverse", BUILTIN_REVERSE},   {"assert", BUILTIN_ASSERT},
    {"panic", BUILTIN_PANIC},
};

static bool is_format(Builtin builtin) {
    return builtin == BUILTIN_LEFT || builtin == BUILTIN_RIGHT || builtin == BUILTIN_FIXED;
}

/* Which built-in function on growable arrays a call is of, if any. */
static Builtin builtin_called(const Checker *c, const Expr *expr) {
    for (size_t i = 0; i < sizeof builtin_names / sizeof builtin_names[0]; i++) {
        if (is_builtin(c, expr, builtin_names[i].name)) return builtin_names[i].builtin;
    }
    return BUILTIN_NONE;
}

/* Checks the arguments of a call that has gone wrong, so that what is in
   them is still looked at. */
static void check_args_anyway(Checker *c, Expr *expr, size_t from) {
    for (size_t i = from; i < expr->as.call.count; i++) {
        if (expr->as.call.args[i]->kind == EXPR_KEY) continue;
        check_operand(c, expr->as.call.args[i]);
    }
}

/* Whether the first argument of a built-in function, already checked as
   type, is a growable array; the problem is reported if not. */
static bool is_growable_operand(Checker *c, Expr *arg, TypeKind type, const char *name) {
    ContainerKind kind = type_container(type);
    if (kind == CONTAINER_LIST) return true;
    if (kind == CONTAINER_MAP) {
        error(c, arg->loc, "'%s' needs a growable array, and %s is a map", name, quoted(c, arg));
    } else if (kind != CONTAINER_NONE) {
        error(c, arg->loc, "'%s' needs a growable array, and %s %s", name, quoted(c, arg),
              kind == CONTAINER_SLICE ? "is a parameter's view of an array; to grow the "
                                        "caller's array, declare the parameter with '[..]'"
                                      : "has a fixed length; declare it with '[..]' to grow it");
    } else if (type != TYPE_INVALID) {
        error(c, arg->loc, "'%s' needs a growable array, found '%s'", name, type_name(type));
    }
    return false;
}

/* Checks something written as text: by cout or cerr, or appended to an
   array of char. It may be any value, a whole array or map, or a value
   given a width or a number of digits by left, right or fixed. */
static void check_written(Checker *c, Expr *arg, const char *doing) {
    Builtin which = arg->kind == EXPR_CALL ? builtin_called(c, arg) : BUILTIN_NONE;
    if (!is_format(which)) {
        TypeKind type = check_operand(c, arg);
        if (type == TYPE_VOID) {
            error(c, arg->loc, "cannot %s a 'void' value", doing);
        } else if (type_is_function(type) && arg->kind == EXPR_IDENT && arg->as.ident.symbol &&
                   arg->as.ident.symbol->kind == SYM_FUNC) {
            error(c, arg->loc, "'%.*s' is a function; call it with '()'",
                  NAME(arg->as.ident.name));
        } else if (type_is_function(type)) {
            error(c, arg->loc, "cannot %s a pointer to a function", doing);
        }
        return;
    }
    const char *name = which == BUILTIN_LEFT ? "left" : which == BUILTIN_RIGHT ? "right" : "fixed";
    arg->as.call.builtin = which;
    arg->type = TYPE_VOID;
    if (arg->as.call.count != 2) {
        error(c, arg->loc, "'%s' takes 2 arguments, but %zu %s given", name,
              arg->as.call.count, arg->as.call.count == 1 ? "was" : "were");
        check_args_anyway(c, arg, 0);
        return;
    }
    Expr *value = arg->as.call.args[0];
    Expr *amount = arg->as.call.args[1];
    if (which == BUILTIN_FIXED) {
        TypeKind found = check_expr(c, value, TYPE_F64);
        if (found != TYPE_INVALID && !type_is_numeric(found)) {
            error(c, value->loc, "'fixed' writes a number, found '%s'", type_name(found));
        }
    } else {
        check_written(c, value, doing);
    }
    TypeKind found = check_expr(c, amount, TYPE_INT);
    if (found != TYPE_INVALID &&
        (!type_is_integer(found) || found == TYPE_I128 || found == TYPE_U128)) {
        error(c, amount->loc, "'%s' takes %s, an integer, found '%s'", name,
              which == BUILTIN_FIXED ? "how many digits to write after the point"
                                     : "how many characters wide to write",
              type_name(found));
    }
}

/* True for a type whose values sort and reverse can put in order. */
static bool orderable(TypeKind type) {
    return type_is_numeric(type) || type == TYPE_CHAR || type == TYPE_BOOL ||
           type_is_enum(type) || type == TYPE_STRING;
}

/* sort(xs), sort(rows, .field) and reverse(xs), on an array of any kind
   whose elements can be changed. */
static TypeKind check_order_builtin(Checker *c, Expr *expr, Builtin which, const char *name) {
    size_t count = expr->as.call.count;
    Expr *first = count ? expr->as.call.args[0] : NULL;
    TypeKind found = first ? check_operand(c, first) : TYPE_INVALID;
    if (!any_array(found)) {
        if (!first) {
            error(c, expr->loc, "'%s' needs an array", name);
        } else if (found != TYPE_INVALID) {
            error(c, first->loc, "'%s' needs an array, found '%s'", name, type_name(found));
        }
        check_args_anyway(c, expr, 1);
        return TYPE_VOID;
    }
    const Symbol *root = place_root(first);
    if (root && (root->kind == SYM_CONST || root->read_only)) {
        error(c, first->loc, "%s array '%.*s' cannot be put in order, since that changes it",
              root->kind == SYM_CONST ? "constant" : "read-only", NAME(root->name));
    }
    if (!report_shared(c, first, "changed")) mark_changed(first);
    size_t most = which == BUILTIN_SORT ? 2 : 1;
    if (count > most) {
        error(c, expr->loc, "'%s' takes %s, but %zu arguments were given", name,
              which == BUILTIN_SORT ? "an array, and the field to sort by" : "an array", count);
        check_args_anyway(c, expr, 1);
        return TYPE_VOID;
    }
    TypeKind elem = type_elem(found);
    if (count == 2) {
        Expr *key = expr->as.call.args[1];
        const TypeInfo *info = type_info(elem);
        if (key->kind == EXPR_IDENT && !key->as.ident.module.len) {
            error(c, key->loc, "'sort' takes the field to sort by as '.%.*s'",
                  NAME(key->as.ident.name));
        } else if (key->kind != EXPR_KEY) {
            check_expr(c, key, TYPE_INVALID);
            error(c, key->loc, "'sort' takes the field to sort by as '.name'");
        } else if (!type_is_struct(elem)) {
            error(c, key->loc, "%s holds '%s', which has no fields to sort by",
                  quoted(c, first), type_name(elem));
        } else {
            key->as.key.index = -1;
            for (size_t i = 0; i < info->field_count; i++) {
                if (str_eq(info->fields[i].name, key->as.key.name)) key->as.key.index = (int)i;
            }
            if (key->as.key.index < 0) {
                error(c, key->loc, "'%s' has no field '%.*s'", type_name(elem),
                      NAME(key->as.key.name));
            } else if (!orderable(info->fields[key->as.key.index].type)) {
                error(c, key->loc, "field '%.*s' is '%s', which has no order to sort by",
                      NAME(key->as.key.name), type_name(info->fields[key->as.key.index].type));
            }
        }
    } else if (which == BUILTIN_SORT && elem != TYPE_INVALID && !orderable(elem)) {
        error(c, first->loc, "'%s' has no order to sort '%s' by%s", name, type_name(elem),
              type_is_struct(elem) ? "; name a field to sort by: 'sort(rows, .field)'" : "");
    }
    return TYPE_VOID;
}

/* Checks a call of a built-in function on a map: has, remove, clear or
   delete. */
static TypeKind check_map_builtin(Checker *c, Expr *expr, Builtin which, const char *name,
                                  TypeKind found) {
    size_t count = expr->as.call.count;
    Expr *first = count ? expr->as.call.args[0] : NULL;
    bool map = type_container(found) == CONTAINER_MAP;
    if (!map && first) {
        if (type_is_container(found)) {
            error(c, first->loc, "'%s' needs a map, and %s is an array", name, quoted(c, first));
        } else if (found != TYPE_INVALID) {
            error(c, first->loc, "'%s' needs a map, found '%s'", name, type_name(found));
        }
    } else if (!first) {
        error(c, expr->loc, "'%s' needs a map", name);
    }
    if (map && which != BUILTIN_HAS && !report_shared(c, first, "changed")) mark_changed(first);
    size_t wanted = which == BUILTIN_HAS || which == BUILTIN_REMOVE ? 2 : 1;
    if (count != wanted) {
        error(c, expr->loc, "'%s' takes %zu argument%s, but %zu %s given", name, wanted,
              wanted == 1 ? "" : "s", count, count == 1 ? "was" : "were");
        check_args_anyway(c, expr, 1);
    } else if (wanted == 2) {
        Expr *key = expr->as.call.args[1];
        TypeKind want = type_key(found);
        TypeKind key_found = check_expr(c, key, want);
        if (want != TYPE_INVALID && key_found != TYPE_INVALID && key_found != want) {
            error(c, key->loc, "a key of map %s must be '%s', found '%s'", quoted(c, first),
                  type_name(want), type_name(key_found));
        }
    }
    return which == BUILTIN_HAS ? TYPE_BOOL : TYPE_VOID;
}

/* Checks a call of a built-in function on growable arrays other than
   clone, which only starts or replaces an array (see check_clone). */
static TypeKind check_builtin(Checker *c, Expr *expr, Builtin which) {
    const char *name = builtin_names[0].name;
    for (size_t i = 0; i < sizeof builtin_names / sizeof builtin_names[0]; i++) {
        if (builtin_names[i].builtin == which) name = builtin_names[i].name;
    }
    expr->as.call.builtin = which;
    size_t count = expr->as.call.count;
    if (is_format(which)) {
        error(c, expr->loc, "'%s' writes a value for 'cout', 'cerr' or 'append' to a char "
                            "array, and gives no value of its own",
              name);
        check_args_anyway(c, expr, 0);
        return TYPE_INVALID;
    }
    if (which == BUILTIN_SORT || which == BUILTIN_REVERSE) {
        return check_order_builtin(c, expr, which, name);
    }
    if (which == BUILTIN_ASSERT) {
        if (count != 1) {
            error(c, expr->loc, "'assert' takes the condition that must hold, but %zu "
                                "arguments were given",
                  count);
            check_args_anyway(c, expr, 0);
            return TYPE_VOID;
        }
        check_condition(c, expr->as.call.args[0]);
        return TYPE_VOID;
    }
    if (which == BUILTIN_PANIC) {
        if (count == 0) error(c, expr->loc, "'panic' takes the message to stop with");
        for (size_t i = 0; i < count; i++) check_written(c, expr->as.call.args[i], "print");
        return TYPE_VOID;
    }
    if (which == BUILTIN_EXIT) {
        if (count != 1) {
            error(c, expr->loc, "'exit' takes the program's exit status, but %zu "
                                "arguments were given",
                  count);
            check_args_anyway(c, expr, 0);
            return TYPE_VOID;
        }
        TypeKind found = check_expr(c, expr->as.call.args[0], TYPE_INT);
        if (found != TYPE_INVALID && !type_is_integer(found)) {
            error(c, expr->as.call.args[0]->loc, "'exit' takes an integer, found '%s'",
                  type_name(found));
        }
        return TYPE_VOID;
    }
    if (which == BUILTIN_CLONE) {
        error(c, expr->loc, "'clone' makes a growable array, so it can only start one or "
                            "replace one: 'int copy[..] = clone(values)'");
        check_args_anyway(c, expr, 0);
        return TYPE_INVALID;
    }
    if (!count) {
        error(c, expr->loc, which == BUILTIN_HAS || which == BUILTIN_REMOVE
                                ? "'%s' needs a map"
                                : "'%s' needs a growable array",
              name);
        return which == BUILTIN_HAS ? TYPE_BOOL : TYPE_VOID;
    }
    Expr *first = expr->as.call.args[0];
    TypeKind found = check_operand(c, first);
    if (which == BUILTIN_HAS || which == BUILTIN_REMOVE ||
        ((which == BUILTIN_CLEAR || which == BUILTIN_DELETE) &&
         type_container(found) == CONTAINER_MAP)) {
        return check_map_builtin(c, expr, which, name, found);
    }
    bool growable = is_growable_operand(c, first, found, name);
    if (growable && !report_shared(c, first, "changed")) mark_changed(first);
    TypeKind elem = growable ? type_elem(found) : TYPE_INVALID;
    size_t wanted = which == BUILTIN_RESERVE ? 2 : 1;
    if (which == BUILTIN_APPEND ? count < 2 : count != wanted) {
        if (which == BUILTIN_APPEND) {
            error(c, expr->loc, "'append' takes a growable array and the values to add");
        } else {
            error(c, expr->loc, "'%s' takes %zu argument%s, but %zu %s given", name, wanted,
                  wanted == 1 ? "" : "s", count, count == 1 ? "was" : "were");
        }
        check_args_anyway(c, expr, 1);
        return which == BUILTIN_POP ? elem : TYPE_VOID;
    }
    for (size_t i = 1; i < count; i++) {
        Expr *arg = expr->as.call.args[i];
        if (which == BUILTIN_RESERVE) {
            TypeKind found = check_expr(c, arg, TYPE_INT);
            if (found != TYPE_INVALID && (!type_is_integer(found) || found == TYPE_I128 ||
                                          found == TYPE_U128)) {
                error(c, arg->loc, "'reserve' takes a count of elements, an integer of at "
                                   "most 64 bits, found '%s'",
                      type_name(found));
            }
            continue;
        }
        if (elem == TYPE_CHAR) {
            /* An array of char takes any value, written as text. */
            check_written(c, arg, "append");
            continue;
        }
        TypeKind value = check_value(c, arg, elem);
        if (elem != TYPE_INVALID && value != TYPE_INVALID && value != elem) {
            error(c, arg->loc, "a value appended to %s must be '%s', found '%s'", quoted(c, first),
                  type_name(elem), type_name(value));
        }
    }
    return which == BUILTIN_POP ? elem : TYPE_VOID;
}

/* Checks "clone(source)", a new growable array of elem holding a copy of
   an array's elements, or of a string's bytes for an array of char. */
static void check_clone(Checker *c, Expr *value, TypeKind elem) {
    value->as.call.builtin = BUILTIN_CLONE;
    value->type = TYPE_VOID;
    if (value->as.call.count != 1) {
        error(c, value->loc, "'clone' takes 1 argument, but %zu were given",
              value->as.call.count);
        check_args_anyway(c, value, 0);
        return;
    }
    Expr *source = value->as.call.args[0];
    TypeKind found = check_operand(c, source);
    if (any_array(found) && type_owns(type_elem(found))) {
        error(c, source->loc, "an array of '%s' cannot be cloned, since each '%s' holds a "
                              "growable array or a map, which a copy would share",
              type_name(type_elem(found)), type_name(type_elem(found)));
        return;
    }
    if (any_array(found)) {
        if (elem != TYPE_INVALID && type_elem(found) != TYPE_INVALID && type_elem(found) != elem) {
            error(c, source->loc, "cannot make a growable array of '%s' from an array of "
                                  "'%s'",
                  type_name(elem), type_name(type_elem(found)));
        }
        return;
    }
    if (found == TYPE_STRING && elem == TYPE_CHAR) return;
    if (found != TYPE_INVALID) {
        error(c, source->loc, "'clone' copies an array%s, found '%s'",
              elem == TYPE_CHAR ? " or a string" : "", type_name(found));
    }
}

/* Checks a value that goes where one of type is kept: a variable, a
   field, an element or a map's value. An array or a map there is checked
   as a whole, and one that would share what another holds is reported:
   it must be new, as a copy made with clone is. Returns its type. */
static TypeKind check_value(Checker *c, Expr *value, TypeKind type) {
    if (value->kind == EXPR_CALL && builtin_called(c, value) == BUILTIN_CLONE) {
        if (type_container(type) == CONTAINER_LIST) {
            check_clone(c, value, type_elem(type));
            value->type = type;
            return type;
        }
        if (type != TYPE_INVALID) {
            error(c, value->loc, "'clone' makes a growable array, and '%s' is wanted here",
                  type_name(type));
        } else {
            check_clone(c, value, TYPE_INVALID);
        }
        return TYPE_INVALID;
    }
    bool want_array = c->want_array;
    c->want_array = type_is_container(type);
    TypeKind found = check_expr(c, value, type);
    c->want_array = want_array;
    if (found != type || type == TYPE_INVALID || !type_owns(type) || is_fresh(value)) {
        return found;
    }
    if (type_container(type) == CONTAINER_LIST && any_array(found)) {
        Str name = value->kind == EXPR_IDENT ? value->as.ident.name : (Str){"...", 3};
        error(c, value->loc, "a growable array has elements of its own; copy those of %s with "
                             "'clone(%.*s)'",
              quoted(c, value), NAME(name));
    } else if (type_is_container(type)) {
        error(c, value->loc, "a '%s' holds what a copy would share, so %s cannot be copied; "
                             "make a new one",
              type_name(type), quoted(c, value));
    } else {
        report_copy(c, value->loc, type, "make a new one with '{ ... }'");
    }
    return TYPE_INVALID;
}

static TypeKind check_cast(Checker *c, Expr *expr) {
    if (expr->as.cast.to_ref) {
        expr->as.cast.to = resolve_type(c, expr->as.cast.to_ref);
        expr->as.cast.to_ref = NULL;
    }
    TypeKind to = expr->as.cast.to;
    Expr *operand = expr->as.cast.operand;
    /* string(text) for a growable array of char is a view of its
       characters: all of them, since it knows how many it holds. */
    /* A struct type in front of "{ ... }" is what gives that value its
       type, as in C's "(Point){ 1, 2 }". */
    bool hinted = type_is_numeric(to) || type_is_struct(to) || type_is_pointer(to);
    TypeKind from;
    if (to == TYPE_STRING) {
        from = check_operand(c, operand);
        if (type_container(from) == CONTAINER_LIST) {
            if (type_elem(from) != TYPE_CHAR) {
                error(c, operand->loc, "cannot make a string of an array of '%s'; only an "
                                       "array of 'char' holds text",
                      type_name(type_elem(from)));
            }
            return TYPE_STRING;
        }
        if (type_is_container(from)) from = operand->type = as_value(c, operand, from);
    } else {
        from = check_expr(c, operand, hinted ? to : TYPE_INVALID);
    }
    if (from == TYPE_INVALID || to == TYPE_INVALID) return to;

    bool allowed = from == to ||
                   (type_is_numeric(from) && type_is_numeric(to)) ||
                   (from == TYPE_CHAR && type_is_integer(to)) ||
                   (type_is_integer(from) && to == TYPE_CHAR) ||
                   (type_is_enum(from) && type_is_integer(to)) ||
                   (type_is_integer(from) && type_is_enum(to)) ||
                   (is_char_pointer(from) && to == TYPE_STRING) ||
                   (type_is_pointer(from) && type_is_pointer(to)) ||
                   (type_is_pointer(from) && to == TYPE_UINTPTR) ||
                   (from == TYPE_UINTPTR && type_is_pointer(to));
    if (!allowed || to == TYPE_VOID) {
        error(c, expr->loc, "cannot convert '%s' to '%s'%s", type_name(from), type_name(to),
              from == TYPE_STRING && is_char_pointer(to)
                  ? "; cstring() makes a copy that ends with a '\\0'"
                  : "");
    }
    return to;
}

static TypeKind check_expr_kind(Checker *c, Expr *expr, TypeKind hint) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
        return check_int_literal(c, expr, hint, false);
    case EXPR_FLOAT_LIT:
        if (type_is_integer(hint)) {
            error(c, expr->loc, "float literal cannot be used as '%s'", type_name(hint));
            return TYPE_INVALID;
        }
        return type_is_float(hint) ? hint : TYPE_FLOAT;
    case EXPR_CHAR_LIT:
        return TYPE_CHAR;
    case EXPR_STRING_LIT:
        /* Where C's kind of string is asked for, a literal is one. */
        return is_char_pointer(hint) ? hint : TYPE_STRING;
    case EXPR_BOOL_LIT:
        return TYPE_BOOL;
    case EXPR_NULL:
        if (!type_is_pointer(hint) && !type_is_function(hint)) {
            error(c, expr->loc, "nothing here tells NULL which pointer type it is; use it "
                                "with a pointer, as in 'int *p = NULL'");
            return TYPE_INVALID;
        }
        return hint;
    case EXPR_IDENT: {
        if (expr->as.ident.module.len) {
            /* "a.b" is a field of the variable a unless a is a module. */
            const Symbol *owner = lookup(c, expr->as.ident.module);
            if (owner && owner->kind != SYM_MODULE) {
                Expr *base = arena_alloc(c->arena, sizeof *base);
                memset(base, 0, sizeof *base);
                base->kind = EXPR_IDENT;
                base->loc = expr->as.ident.module_loc;
                base->type = TYPE_INVALID;
                base->as.ident.name = expr->as.ident.module;
                base->as.ident.name_loc = expr->as.ident.module_loc;
                Str name = expr->as.ident.name;
                SrcLoc name_loc = expr->as.ident.name_loc;
                expr->kind = EXPR_FIELD;
                expr->as.field.base = base;
                expr->as.field.name = name;
                expr->as.field.name_loc = name_loc;
                expr->as.field.arrow = false;
                expr->as.field.index = -1;
                expr->as.field.through_pointer = false;
                return check_field(c, expr);
            }
        }
        Symbol *symbol = expr->as.ident.module.len
                             ? resolve_member(c, expr->as.ident.module,
                                              expr->as.ident.module_loc,
                                              expr->as.ident.name, expr->as.ident.name_loc)
                             : resolve(c, expr->as.ident.name, expr->loc);
        if (!symbol) return TYPE_INVALID;
        if (symbol->kind == SYM_FUNC && symbol->decl->as.func.variadic) {
            error(c, expr->loc, "'%.*s' takes '...', so it can only be called, not used as "
                                "a value",
                  NAME(expr->as.ident.name));
            return TYPE_INVALID;
        }
        if (symbol->kind == SYM_FUNC) {
            /* A pointer to the function. */
            symbol->used = true;
            expr->as.ident.symbol = symbol;
            return function_type(c, symbol->decl);
        }
        if (symbol->kind == SYM_TYPE) {
            symbol->used = true;
            error(c, expr->loc, "'%.*s' is a type, not a value", NAME(expr->as.ident.name));
            return TYPE_INVALID;
        }
        if (symbol->kind == SYM_MODULE) {
            symbol->used = true;
            error(c, expr->loc, "'%.*s' is a module; use what it exports as '%.*s.name'",
                  NAME(expr->as.ident.name), NAME(expr->as.ident.name));
            return TYPE_INVALID;
        }
        symbol->used = true;
        expr->as.ident.symbol = symbol;
        return symbol->type;
    }
    case EXPR_INDEX:
        return check_index(c, expr);
    case EXPR_SLICE:
        return check_slice(c, expr);
    case EXPR_KEY:
        error(c, expr->loc, "'.%.*s' names a field to sort by, as in 'sort(rows, .%.*s)', and "
                            "has no value of its own",
              NAME(expr->as.key.name), NAME(expr->as.key.name));
        return TYPE_INVALID;
    case EXPR_FIELD:
        return check_field(c, expr);
    case EXPR_INIT:
        return check_init(c, expr, hint);
    case EXPR_LEN:
        return TYPE_INT;
    case EXPR_UNARY:
        return check_unary(c, expr, hint);
    case EXPR_BINARY:
        return check_binary(c, expr, hint);
    case EXPR_CALL:
        if (is_builtin(c, expr, "len")) return check_len(c, expr);
        if (builtin_called(c, expr)) return check_builtin(c, expr, builtin_called(c, expr));
        if (is_builtin(c, expr, "cstring")) return check_cstring(c, expr);
        return check_call(c, expr);
    case EXPR_CAST:
        return check_cast(c, expr);
    case EXPR_TERNARY:
        return check_ternary(c, expr, hint);
    case EXPR_CIN_OK:
        return TYPE_BOOL;
    case EXPR_SIZEOF: {
        /* A size is known here, so the node becomes an integer literal: it
           then adapts to its context like any other, and the later passes
           never see a sizeof. */
        TypeKind of = expr->as.size_of.type;
        if (expr->as.size_of.type_ref) of = resolve_type(c, expr->as.size_of.type_ref);
        Symbol *named = expr->as.size_of.operand
                            ? type_named_by(c, expr->as.size_of.operand)
                            : NULL;
        if (named) {
            use_type_name(c, expr->as.size_of.operand, named);
            of = named->type;
        } else if (expr->as.size_of.operand) {
            Expr *operand = expr->as.size_of.operand;
            of = check_operand(c, operand);
            if (type_container(of) == CONTAINER_LIST) {
                Str name = operand->kind == EXPR_IDENT ? operand->as.ident.name
                                                       : (Str){"xs", 2};
                error(c, expr->loc,
                      "the size of growable array %s changes as it grows; "
                      "'len(%.*s) * sizeof(%s)' is the size of its elements",
                      quoted(c, operand), NAME(name), type_name(type_elem(of)));
                of = TYPE_INVALID;
            }
        }
        if (of == TYPE_VOID) error(c, expr->loc, "cannot take the size of 'void'");
        const TypeInfo *sized = type_info(of);
        if (sized && sized->incomplete) {
            error(c, expr->loc, "the size of '%s' is not known above its declaration",
                  type_name(of));
        }
        expr->kind = EXPR_INT_LIT;
        expr->as.int_lit = type_size(of);
        return check_int_literal(c, expr, hint, false);
    }
    }
    return TYPE_INVALID;
}

/* hint is the type the context would like, or TYPE_INVALID for none. It only
   steers untyped literals; the caller still compares the result. */

/* The array or map, if any, that a place is an element or a value of and
   that moves its elements as it grows: a growable array or a map, the
   first such on the way from the place to the variable it is part of. */
static const Expr *moving_part(const Expr *expr) {
    for (;;) {
        if (expr->kind == EXPR_FIELD && !expr->as.field.through_pointer) {
            expr = expr->as.field.base;
        } else if (expr->kind == EXPR_INDEX && type_is_container(expr->as.index.base->type)) {
            ContainerKind kind = type_container(expr->as.index.base->type);
            if (kind == CONTAINER_LIST || kind == CONTAINER_MAP) return expr->as.index.base;
            expr = expr->as.index.base;
        } else {
            return NULL;
        }
    }
}

/* Notes that a place is changed: what it is a value of in a map is put in
   the map, if its key is missing. */
static void mark_changed(Expr *expr) {
    for (;;) {
        if (expr->kind == EXPR_FIELD && !expr->as.field.through_pointer) {
            expr = expr->as.field.base;
        } else if (expr->kind == EXPR_INDEX && type_is_container(expr->as.index.base->type)) {
            if (type_container(expr->as.index.base->type) == CONTAINER_MAP) {
                expr->as.index.inserts = true;
            }
            expr = expr->as.index.base;
        } else {
            return;
        }
    }
}

/* A whole array or map used where a value goes. A fixed array, or a view
   of one, stands for a pointer to its first element, as in C, when that
   pointer could not outlive or misuse it; anything else is reported.
   Returns the type it is used as. */
static TypeKind as_value(Checker *c, Expr *expr, TypeKind type) {
    ContainerKind kind = type_container(type);
    Str name = expr->kind == EXPR_IDENT ? expr->as.ident.name : (Str){0};
    if (kind == CONTAINER_MAP) {
        if (name.len) {
            error(c, expr->loc,
                  "map '%.*s' cannot be used as a value; look up a key with '%.*s[key]', "
                  "or pass it to a map parameter",
                  NAME(name), NAME(name));
        } else {
            error(c, expr->loc, "%s is a map; look up a key with '[key]'", quoted(c, expr));
        }
        return TYPE_INVALID;
    }
    if (kind == CONTAINER_LIST) {
        if (name.len) {
            error(c, expr->loc,
                  "growable array '%.*s' cannot be used as a pointer, since its elements "
                  "move when it grows; index it, or pass it to an array parameter",
                  NAME(name));
        } else {
            error(c, expr->loc, "%s is a growable array; index it, or use it where an array "
                                "goes, as in 'len(...)' or 'append(...)'",
                  quoted(c, expr));
        }
        return TYPE_INVALID;
    }
    const Symbol *root = place_root(expr);
    if (root && root->read_only) {
        error(c, expr->loc, "read-only array %s cannot be used as a pointer, which could "
                            "change it; index it, or pass it on to a 'const' array parameter",
              quoted(c, expr));
        return TYPE_INVALID;
    }
    if (root && root->kind == SYM_CONST) {
        error(c, expr->loc, "constant array %s cannot be used as a value, since the pointer "
                            "it would be could change it; index it, print it, or take its "
                            "length",
              quoted(c, expr));
        return TYPE_INVALID;
    }
    const Expr *mover = moving_part(expr);
    if (mover) {
        error(c, expr->loc, "%s moves when %s grows, so it cannot be used as a pointer; "
                            "index it, or pass it to an array parameter",
              quoted(c, expr), quoted(c, mover));
        return TYPE_INVALID;
    }
    if (!through_read_only(expr) && report_shared(c, expr, "pointed to")) return TYPE_INVALID;
    if (type_elem(type) == TYPE_INVALID) return TYPE_INVALID;
    expr->decayed = true;
    expr->decayed_from = type;
    return through_read_only(expr) ? type_read_only_pointer_to(type_elem(type))
                                   : type_pointer_to(type_elem(type));
}

/* True if a value of type from may go where one of type to is wanted with
   no conversion written: any pointer as a "void *", and a pointer as a
   read-only one to what it points to, as C allows. */
static bool converts_to(TypeKind from, TypeKind to) {
    if (from == to || !type_is_pointer(from) || !type_is_pointer(to)) return false;
    bool loses = type_is_read_only_pointer(from) && !type_is_read_only_pointer(to);
    if (type_target(to) == TYPE_VOID) return !loses;
    return type_is_read_only_pointer(to) && type_target(from) == type_target(to);
}

static TypeKind check_expr(Checker *c, Expr *expr, TypeKind hint) {
    if (is_untyped(expr) && !type_is_numeric(hint)) {
        hint = has_float_literal(expr) ? TYPE_FLOAT : TYPE_INT;
    }
    /* A field is looked at once, and keeps the type it was found to have. */
    if (expr->kind == EXPR_FIELD && expr->checked) return expr->type;
    bool want_array = c->want_array;
    c->want_array = false;
    TypeKind type = check_expr_kind(c, expr, hint);
    c->want_array = want_array;
    expr->type = type;
    if (type_is_container(type) && !want_array) type = expr->type = as_value(c, expr, type);
    if (expr->kind == EXPR_FIELD) expr->checked = true;
    return converts_to(type, hint) ? hint : type;
}

/* If the operand of len() has a length known at compile time, sets *length
   and returns true: a fixed array, a string literal, or a string constant. */
static bool const_len(const Expr *operand, size_t *length) {
    if (type_container(operand->type) == CONTAINER_ARRAY && !operand->decayed) {
        *length = type_length(operand->type);
        return true;
    }
    while (operand->kind == EXPR_IDENT) {
        const Symbol *symbol = operand->as.ident.symbol;
        if (!symbol) return false;
        if (type_container(symbol->type) == CONTAINER_ARRAY) {
            *length = type_length(symbol->type);
            return true;
        }
        if (symbol->kind != SYM_CONST || type_is_container(symbol->type) ||
            symbol->decl->external || symbol->decl->as.decl.broken) {
            return false;
        }
        operand = symbol->decl->as.decl.init;
    }
    if (operand->kind != EXPR_STRING_LIT) return false;
    *length = operand->as.string_lit.len;
    return true;
}

/* True if the value is known at compile time. */
static bool is_constant(const Expr *expr) {
    size_t length;
    switch (expr->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_NULL:
        return true;
    case EXPR_IDENT:
        return !expr->as.ident.symbol || expr->as.ident.symbol->kind == SYM_CONST ||
               expr->as.ident.symbol->kind == SYM_ENUM_VALUE;
    case EXPR_UNARY:
        /* Where a variable is, and what a pointer points to, are only known
           when the program runs. */
        if (expr->as.unary.op == OP_ADDR || expr->as.unary.op == OP_DEREF) return false;
        return is_constant(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_constant(expr->as.binary.left) && is_constant(expr->as.binary.right);
    case EXPR_CAST:
        /* Converting between the two kinds of string is work done when the
           program runs. */
        if (expr->type != expr->as.cast.operand->type &&
            (expr->type == TYPE_STRING || expr->as.cast.operand->type == TYPE_STRING)) {
            return false;
        }
        return is_constant(expr->as.cast.operand);
    case EXPR_TERNARY:
        /* A string or struct constant is stored, not computed where it is
           used, so its value must be settled here; see settle_constant. */
        if (expr->type == TYPE_STRING || type_is_struct(expr->type)) return false;
        return is_constant(expr->as.ternary.cond) && is_constant(expr->as.ternary.then) &&
               is_constant(expr->as.ternary.otherwise);
    case EXPR_INIT:
        for (size_t i = 0; i < expr->as.init.count; i++) {
            if (!is_constant(expr->as.init.items[i].value)) return false;
        }
        return true;
    case EXPR_FIELD:
        return false;
    case EXPR_LEN:
        return const_len(expr->as.len.operand, &length);
    case EXPR_CALL:
    case EXPR_SIZEOF:
    case EXPR_CIN_OK:
    case EXPR_INDEX:
    case EXPR_SLICE:
    case EXPR_KEY:
        return false;
    }
    return false;
}

/* ---- Statements ---- */

static void check_block(Checker *c, Block *block);

static bool eval_const_int(const Expr *expr, long long *out);

/* Evaluates a bool expression made of literals, constants, comparisons of
   integers and logic. Returns false if it is not one. */
static bool eval_const_bool(const Expr *expr, bool *out) {
    long long left, right;
    bool a, b;
    switch (expr->kind) {
    case EXPR_BOOL_LIT:
        *out = expr->as.bool_lit;
        return true;
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (!symbol || symbol->kind != SYM_CONST || type_is_container(symbol->type) ||
            symbol->decl->external || symbol->decl->as.decl.broken) {
            return false;
        }
        return eval_const_bool(symbol->decl->as.decl.init, out);
    }
    case EXPR_UNARY:
        if (expr->as.unary.op != OP_NOT || !eval_const_bool(expr->as.unary.operand, &a)) {
            return false;
        }
        *out = !a;
        return true;
    case EXPR_TERNARY:
        if (!eval_const_bool(expr->as.ternary.cond, &a)) return false;
        return eval_const_bool(a ? expr->as.ternary.then : expr->as.ternary.otherwise, out);
    case EXPR_BINARY:
        if (expr->as.binary.op == OP_AND || expr->as.binary.op == OP_OR) {
            if (!eval_const_bool(expr->as.binary.left, &a) ||
                !eval_const_bool(expr->as.binary.right, &b)) {
                return false;
            }
            *out = expr->as.binary.op == OP_AND ? a && b : a || b;
            return true;
        }
        if (!eval_const_int(expr->as.binary.left, &left) ||
            !eval_const_int(expr->as.binary.right, &right)) {
            return false;
        }
        switch (expr->as.binary.op) {
        case OP_EQ: *out = left == right; return true;
        case OP_NE: *out = left != right; return true;
        case OP_LT: *out = left < right;  return true;
        case OP_LE: *out = left <= right; return true;
        case OP_GT: *out = left > right;  return true;
        case OP_GE: *out = left >= right; return true;
        default:    return false;
        }
    default:
        return false;
    }
}

/* Evaluates an integer expression made of literals, constants and
   arithmetic. Returns false if it is not one, or overflows. */
static bool eval_const_int(const Expr *expr, long long *out) {
    long long left, right;
    bool chosen;
    size_t length;
    switch (expr->kind) {
    case EXPR_LEN:
        if (!const_len(expr->as.len.operand, &length)) return false;
        *out = (long long)length;
        return true;
    case EXPR_TERNARY:
        if (!eval_const_bool(expr->as.ternary.cond, &chosen)) return false;
        return eval_const_int(chosen ? expr->as.ternary.then : expr->as.ternary.otherwise,
                              out);
    case EXPR_INT_LIT:
        if (expr->as.int_lit > (U128)0x7fffffffffffffffLL) return false;
        *out = (long long)expr->as.int_lit;
        return true;
    case EXPR_CHAR_LIT:
        *out = (unsigned char)expr->as.char_lit;
        return true;
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (symbol && symbol->kind == SYM_ENUM_VALUE) {
            *out = symbol->value;
            return true;
        }
        if (!symbol || symbol->kind != SYM_CONST || type_is_container(symbol->type) ||
            symbol->decl->external || symbol->decl->as.decl.broken) {
            return false;
        }
        return eval_const_int(symbol->decl->as.decl.init, out);
    }
    case EXPR_CAST:
        return eval_const_int(expr->as.cast.operand, out);
    case EXPR_UNARY:
        if (!eval_const_int(expr->as.unary.operand, &right)) return false;
        if (expr->as.unary.op == OP_BIT_NOT) {
            *out = ~right;
            return true;
        }
        if (expr->as.unary.op != OP_NEG) return false;
        return !__builtin_sub_overflow(0LL, right, out);
    case EXPR_BINARY:
        if (!eval_const_int(expr->as.binary.left, &left) ||
            !eval_const_int(expr->as.binary.right, &right)) {
            return false;
        }
        switch (expr->as.binary.op) {
        case OP_ADD: return !__builtin_add_overflow(left, right, out);
        case OP_SUB: return !__builtin_sub_overflow(left, right, out);
        case OP_MUL: return !__builtin_mul_overflow(left, right, out);
        case OP_DIV:
        case OP_MOD:
            if (right == 0 || right == -1) return false;
            *out = expr->as.binary.op == OP_DIV ? left / right : left % right;
            return true;
        case OP_BIT_AND: *out = left & right; return true;
        case OP_BIT_OR:  *out = left | right; return true;
        case OP_BIT_XOR: *out = left ^ right; return true;
        case OP_SHL:
        case OP_SHR:
            /* Kept to amounts and values that cannot overflow here. */
            if (left < 0 || right < 0 || right > 62) return false;
            if (expr->as.binary.op == OP_SHR) {
                *out = left >> right;
                return true;
            }
            if (left > (0x7fffffffffffffffLL >> right)) return false;
            *out = left << right;
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* The value of a constant whose choice between alternatives can be made at
   compile time: the branch that a conditional with a constant condition
   picks, however deep; any other expression unchanged. */
static Expr *settle_constant(Expr *expr) {
    bool chosen;
    while (expr->kind == EXPR_TERNARY && eval_const_bool(expr->as.ternary.cond, &chosen)) {
        expr = chosen ? expr->as.ternary.then : expr->as.ternary.otherwise;
    }
    return expr;
}

/* Keeps a mistyped length from asking for an absurd amount of memory. */
#define MAX_ARRAY_LENGTH (1LL << 28)

/* True for a type a map's keys can have: one whose values compare equal
   exactly when they are the same, byte for byte, or a string. */
static bool valid_key(TypeKind type) {
    return type_is_integer(type) || type == TYPE_CHAR || type == TYPE_BOOL ||
           type_is_enum(type) || type_is_pointer(type) || type == TYPE_STRING;
}

static void check_key_type(Checker *c, TypeKind key, SrcLoc loc) {
    if (key != TYPE_INVALID && !valid_key(key)) {
        error(c, loc, "a map's keys cannot be '%s'; they can be integers, 'char', 'bool', "
                      "enums, pointers or strings",
              type_name(key));
    }
}

/* True for a type whose values hold a growable array or a map, and so
   cannot be copied: the copy would share its elements. */
static bool type_owns(TypeKind type) {
    switch (type_container(type)) {
    case CONTAINER_LIST:
    case CONTAINER_MAP:
        return true;
    case CONTAINER_ARRAY:
        return type_owns(type_elem(type));
    case CONTAINER_SLICE:
        return false;
    default: {
        const TypeInfo *info = type_info(type);
        return info && info->owns;
    }
    }
}

/* True for a value of an owning struct that is new, and so can be stored
   where it goes: one written out with "{ ... }", or one taken out of a
   growable array with pop. */
static bool is_fresh(const Expr *value) {
    return value->kind == EXPR_INIT ||
           (value->kind == EXPR_CALL && (value->as.call.builtin == BUILTIN_POP ||
                                         value->as.call.builtin == BUILTIN_CLONE));
}

static void report_copy(Checker *c, SrcLoc loc, TypeKind type, const char *instead) {
    error(c, loc, "a '%s' holds a growable array or a map, so it cannot be copied; %s",
          type_name(type), instead);
}

/* Where suffixes are written, which settles what "[]" means. */
typedef enum {
    SUFFIXES_DECL,  /* "[]" first: an array as long as its elements */
    SUFFIXES_PARAM, /* "[]" first: a view of the caller's elements */
    SUFFIXES_FIELD, /* no "[]" */
    SUFFIXES_EACH,  /* a loop variable, as a parameter */
} SuffixUse;

/* The type that a name's suffixes make of type, the type written before
   the name. The last suffix applies first, as in C, so "int grid[3][4]" is
   an array of 3 arrays of 4 ints. given is the number of elements written
   out, for a "[]" whose length they settle. */
static TypeKind apply_suffixes(Checker *c, TypeKind type, Suffix *suffixes, size_t count,
                               SuffixUse use, size_t given, Str name) {
    const char *written = type_name(type);
    for (size_t i = count; i-- > 0;) {
        Suffix *suffix = &suffixes[i];
        bool outer = i == 0;
        TypeKind key = TYPE_INVALID;
        long long value;
        switch (suffix->kind) {
        case SUFFIX_GROWABLE:
            type = type == TYPE_INVALID ? TYPE_INVALID : type_list_of(type);
            continue;
        case SUFFIX_MAP:
            key = declared_type(c, suffix->key, suffix->key_ref, suffix->loc, name);
            break;
        case SUFFIX_OPEN:
            if (outer && (use == SUFFIXES_PARAM || use == SUFFIXES_EACH)) {
                type = type == TYPE_INVALID ? TYPE_INVALID : type_slice_of(type);
            } else if (outer && use == SUFFIXES_DECL) {
                if (given == 0) {
                    error(c, suffix->loc, "an array needs at least one element");
                    type = TYPE_INVALID;
                } else if (type != TYPE_INVALID) {
                    type = type_array_of(type, given);
                }
            } else {
                error(c, suffix->loc,
                      use == SUFFIXES_FIELD
                          ? "a field that is an array needs its length: '[n]', or '[..]' "
                            "for a growable one"
                          : "only the first '[]' can leave out its length");
                type = TYPE_INVALID;
            }
            continue;
        case SUFFIX_LENGTH: {
            /* "V name[K]" with K a type's name, which parsed as a length. */
            Symbol *keyed = type_named_by(c, suffix->length);
            if (keyed) {
                use_type_name(c, suffix->length, keyed);
                key = keyed->type;
                break;
            }
            if (outer && use == SUFFIXES_PARAM) {
                error(c, suffix->loc,
                      "an array parameter is written '%s %.*s[]', without a length, or "
                      "'%s %.*s[..]' for a growable one",
                      written, NAME(name), written, NAME(name));
                check_expr(c, suffix->length, TYPE_INT);
                type = TYPE_INVALID;
                continue;
            }
            TypeKind found = check_expr(c, suffix->length, TYPE_INT);
            if (found == TYPE_INVALID) {
                type = TYPE_INVALID;
            } else if (!type_is_integer(found) || !eval_const_int(suffix->length, &value)) {
                error(c, suffix->length->loc,
                      "an array's length must be an integer known at compile time");
                type = TYPE_INVALID;
            } else if (value < 1 || value > MAX_ARRAY_LENGTH) {
                error(c, suffix->length->loc, "an array's length must be between 1 and %lld",
                      MAX_ARRAY_LENGTH);
                type = TYPE_INVALID;
            } else if (type != TYPE_INVALID) {
                type = type_array_of(type, (size_t)value);
            }
            continue;
        }
        }
        /* A key type that cannot be one is reported, and the map is kept, so
           that its uses are not. */
        check_key_type(c, key, suffix->loc);
        type = type == TYPE_INVALID || key == TYPE_INVALID ? TYPE_INVALID : type_map_of(key, type);
    }
    return type;
}

/* The type of a pointer to a function, as written: "ret (*)(params)". */
static TypeKind signature_type(Checker *c, const FuncSig *sig) {
    TypeKind ret = sig->ret_ref ? resolve_type(c, sig->ret_ref) : sig->ret;
    TypeKind *params = arena_alloc(c->arena, (sig->param_count ? sig->param_count : 1) *
                                                 sizeof *params);
    bool valid = ret != TYPE_INVALID;
    for (size_t i = 0; i < sig->param_count; i++) {
        Param *param = &sig->params[i];
        TypeKind type = declared_type(c, param->type, param->type_ref, param->loc, param->name);
        params[i] = apply_suffixes(c, type, param->suffixes, param->suffix_count,
                                   SUFFIXES_PARAM, 0, param->name);
        if (params[i] == TYPE_INVALID) valid = false;
    }
    return valid ? type_function_of(ret, params, sig->param_count) : TYPE_INVALID;
}

/* Checks the entries a map declaration starts with, "key: value, ...". */
static void check_map_entries(Checker *c, Stmt *stmt, TypeKind type) {
    if (stmt->as.decl.elem_count && !stmt->as.decl.pairs) {
        error(c, stmt->as.decl.elems[0]->loc, "a map's entries are written 'key: value'");
        return;
    }
    for (size_t i = 0; i < stmt->as.decl.elem_count; i++) {
        Expr *elem = stmt->as.decl.elems[i];
        TypeKind want = i % 2 ? type_elem(type) : type_key(type);
        TypeKind found = check_value(c, elem, want);
        if (want != TYPE_INVALID && found != TYPE_INVALID && found != want) {
            error(c, elem->loc, "a %s of this map must be '%s', found '%s'",
                  i % 2 ? "value" : "key", type_name(want), type_name(found));
        }
    }
}

/* Checks "T name[n]", "T name[..]", "V name[K]" and the like, with what
   it starts out holding. */
static void check_array_decl(Checker *c, Stmt *stmt) {
    TypeKind elem = declared_type(c, stmt->as.decl.type, stmt->as.decl.type_ref, stmt->loc,
                                  stmt->as.decl.name);
    bool is_const = stmt->kind == STMT_CONST;
    size_t count = stmt->as.decl.elem_count;
    TypeKind type = apply_suffixes(c, elem, stmt->as.decl.suffixes, stmt->as.decl.suffix_count,
                                   SUFFIXES_DECL, count, stmt->as.decl.name);
    stmt->as.decl.type = type;
    ContainerKind kind = type_container(type);
    bool outer_map = stmt->as.decl.suffixes[0].kind == SUFFIX_MAP || kind == CONTAINER_MAP;
    if (stmt->as.decl.pairs && !outer_map) {
        /* Reported, and declared without them, so that its uses are not. */
        error(c, stmt->as.decl.elems[0]->loc,
              "only a map's entries are written 'key: value'; an array lists its elements");
        stmt->as.decl.elem_count = count = 0;
    }
    if (kind == CONTAINER_MAP) {
        if (is_const) error(c, stmt->loc, "a map cannot be constant; declare it as a variable");
        check_map_entries(c, stmt, type);
    } else if (kind == CONTAINER_LIST && is_const) {
        error(c, stmt->loc, "a constant array cannot be growable, since its elements never "
                            "change; give it a length or leave the length out");
    }
    if (stmt->as.decl.init) {
        TypeKind found = check_value(c, stmt->as.decl.init, type);
        if (type != TYPE_INVALID && found != TYPE_INVALID && found != type) {
            error(c, stmt->as.decl.init->loc, "cannot start '%s' %s with '%s'",
                  type_name(type), is_const ? "constant" : "variable", type_name(found));
        }
        if (is_const) {
            error(c, stmt->as.decl.init->loc, "a constant array needs its elements: "
                                              "'= { ... }'");
        }
    }
    if (type == TYPE_INVALID) {
        /* What is wrong with the type has been reported; elements checked
           against no type would only report more. */
    } else if (kind != CONTAINER_MAP) {
        TypeKind each = type_elem(type);
        for (size_t i = 0; i < count; i++) {
            Expr *value = stmt->as.decl.elems[i];
            TypeKind found = check_value(c, value, each);
            if (is_const) value = stmt->as.decl.elems[i] = settle_constant(value);
            if (each == TYPE_INVALID) continue;
            if (found != TYPE_INVALID && found != each) {
                error(c, value->loc, "an element of this array must be '%s', found '%s'",
                      type_name(each), type_name(found));
            } else if (is_const && !is_constant(value)) {
                error(c, value->loc, "a constant's value must be known at compile time; use "
                                     "a variable for this");
            }
        }
        if (kind == CONTAINER_ARRAY && count > type_length(type)) {
            error(c, stmt->as.decl.elems[type_length(type)]->loc,
                  "too many elements: the array has length %zu", type_length(type));
        }
    }

    if (stmt->is_static && type != TYPE_INVALID) {
        bool constant = !stmt->as.decl.init;
        for (size_t i = 0; constant && i < count; i++) {
            constant = is_constant(stmt->as.decl.elems[i]);
        }
        if (kind == CONTAINER_LIST || kind == CONTAINER_MAP ? stmt->as.decl.init || count
                                                             : !constant) {
            error(c, stmt->loc, "a static %s starts out once, before the program runs, so %s",
                  kind == CONTAINER_MAP ? "map" : "array",
                  kind == CONTAINER_LIST || kind == CONTAINER_MAP
                      ? "it starts empty; add to it in the function"
                      : "its elements must be known at compile time");
        }
    }
    Symbol *symbol = new_symbol(c, is_const ? SYM_CONST : SYM_VAR, stmt->as.decl.name,
                                stmt->loc, type);
    symbol->decl = stmt;
    symbol->exported = stmt->exported;
    symbol->is_static = stmt->is_static;
    stmt->as.decl.symbol = symbol;
    declare(c, symbol);
    if (!is_const) {
        list_push(c, symbol->global || symbol->is_static ? &c->unit->program.globals : c->locals,
                  symbol);
    }
    /* A local array lives on the stack, which a large one overflows with
       no message at all: a machine with 64-bit pointers usually gives a
       program 8 MB of stack, and a small board much less. */
    size_t stack_limit = types_platform()->pointer_size >= 8 ? (size_t)1 << 20 : 1024;
    if (!is_const && !symbol->global && kind == CONTAINER_ARRAY &&
        type_size(type) > stack_limit) {
        diag_add(c->diags, SEV_WARNING, stmt->loc,
                 "array '%.*s' takes %zu bytes of the stack, which may be more than it "
                 "has; declare it at the top level, outside any function",
                 NAME(stmt->as.decl.name), type_size(type));
    }
}

/* Reports a type that cannot pass between Wind and C as it is: one whose
   C form is Wind's own. */
/* For an extern whose header sits beside the file that declares it, reads
   that header, which the library carries and the generated C holds. */
static void carry_header(Checker *c, Stmt *stmt) {
    if (!stmt->header.len || stmt->carried.len) return;
    char *path = path_beside(c->unit->path, stmt->header);
    Str text;
    if (file_exists(path) && !is_directory(path) && file_read(path, &text)) {
        char *copy = arena_alloc(&c->unit->root->arena, text.len + 1);
        memcpy(copy, text.data, text.len);
        copy[text.len] = '\0';
        stmt->carried = (Str){copy, text.len};
        free((void *)text.data);
    }
    free(path);
}

/* Reports a type that cannot pass between Wind and C as it is. A header
   that a library carries is written for Wind, and takes strings as they
   are in Wind too; carried tells whether this is one. */
static void check_crossing_to(Checker *c, TypeKind type, SrcLoc loc, const char *what,
                              bool carried) {
    if (carried && type == TYPE_STRING) return;
    if (type == TYPE_INVALID || type == TYPE_VOID || type_is_numeric(type) ||
        type == TYPE_BOOL || type == TYPE_CHAR || type_is_enum(type) || type_is_pointer(type) ||
        type_is_function(type)) {
        return;
    }
    error(c, loc, "%s cannot have type '%s'; C has no matching type%s", what, type_name(type),
          type == TYPE_STRING ? "; C's strings are 'char *'" : "");
}


static void check_decl(Checker *c, Stmt *stmt) {
    if (stmt->as.decl.is_array) {
        check_array_decl(c, stmt);
        return;
    }
    stmt->as.decl.type = declared_type(c, stmt->as.decl.type, stmt->as.decl.type_ref,
                                       stmt->loc, stmt->as.decl.name);
    TypeKind type = stmt->as.decl.type;
    Expr *init = stmt->as.decl.init;
    bool is_const = stmt->kind == STMT_CONST;
    if (stmt->external) {
        carry_header(c, stmt);
        check_crossing_to(c, type, stmt->loc, "an external constant", stmt->carried.len > 0);
    }

    /* The initializer is checked before the name is declared, so a
       declaration cannot refer to itself. */
    if (init) {
        TypeKind found = check_expr(c, init, type);
        /* "const T *p = &x" is C's pointer that cannot change what it points
           to, a variable; it is a constant as well when its value is one. */
        if (is_const && type_is_read_only_pointer(type) && !stmt->exported && !stmt->external &&
            !is_constant(init)) {
            stmt->kind = STMT_VAR;
            is_const = false;
        }
        if (is_const) init = stmt->as.decl.init = settle_constant(init);
        if (type == TYPE_INVALID) {
            /* the type's own problem is already reported */
        } else if (found != TYPE_INVALID && found != type) {
            error(c, init->loc, "cannot initialize '%s' %s with '%s'", type_name(type),
                  is_const ? "constant" : "variable", type_name(found));
        } else if (is_const && !is_constant(init)) {
            error(c, init->loc,
                  "a constant's value must be known at compile time; use a variable "
                  "for this");
        }
    }

    if (stmt->is_static && init && type != TYPE_INVALID && !is_constant(init)) {
        error(c, init->loc, "a static variable starts out once, before the program runs, so "
                            "its value must be known at compile time");
    }
    Symbol *symbol = new_symbol(c, is_const ? SYM_CONST : SYM_VAR, stmt->as.decl.name,
                                stmt->loc, type);
    symbol->decl = stmt;
    symbol->exported = stmt->exported;
    symbol->is_static = stmt->is_static;
    /* Given an existing struct that holds growable arrays or maps, the
       variable is a copy that shares them, read-only, as a parameter is. */
    symbol->shares = init && type_owns(type) && !is_fresh(init);
    stmt->as.decl.symbol = symbol;
    declare(c, symbol);
    if (!is_const) {
        list_push(c, symbol->global || symbol->is_static ? &c->unit->program.globals : c->locals,
                  symbol);
    }
}

/* The variable or parameter that a place, an array or a map is part of
   without going through a pointer: x for "x", "x.f", "x.f[i]" and
   "x.m[key].g". NULL for any other. */
static const Symbol *place_owner(const Expr *expr) {
    for (;;) {
        if (expr->kind == EXPR_FIELD && !expr->as.field.through_pointer) {
            expr = expr->as.field.base;
        } else if (expr->kind == EXPR_INDEX && type_is_container(expr->as.index.base->type)) {
            expr = expr->as.index.base;
        } else {
            return expr->kind == EXPR_IDENT ? expr->as.ident.symbol : NULL;
        }
    }
}

/* True if a place, an array or a map is reached through a read-only
   pointer, "const T *", and so cannot be changed. */
static bool through_read_only(const Expr *expr) {
    for (;;) {
        switch (expr->kind) {
        case EXPR_FIELD:
            if (expr->as.field.through_pointer) {
                return type_is_read_only_pointer(expr->as.field.base->type);
            }
            expr = expr->as.field.base;
            break;
        case EXPR_INDEX:
            if (type_is_pointer(expr->as.index.base->type)) {
                return type_is_read_only_pointer(expr->as.index.base->type);
            }
            if (!type_is_container(expr->as.index.base->type)) return false;
            expr = expr->as.index.base;
            break;
        case EXPR_UNARY:
            return expr->as.unary.op == OP_DEREF &&
                   type_is_read_only_pointer(expr->as.unary.operand->type);
        default:
            return false;
        }
    }
}

/* Reports, and returns true for, a place, an array or a map that is part of
   a copy that shares its elements: doing is what was to be done to it. */
static bool report_shared(Checker *c, const Expr *expr, const char *doing) {
    if (through_read_only(expr) && strcmp(doing, "pointed to") != 0) {
        error(c, expr->loc, "this is reached through a 'const' pointer, so it cannot be %s",
              doing);
        return true;
    }
    const Symbol *owner = place_owner(expr);
    if (!owner || !owner->shares) return false;
    error(c, expr->loc, "'%.*s' shares its growable arrays and maps with the '%s' it was "
                        "made from, so nothing in it can be %s",
          NAME(owner->name), type_name(owner->type), doing);
    return true;
}

/* True for an expression that stands for a whole array, not a pointer. */
static bool names_array(const Expr *expr) {
    return any_array(expr->type) && !expr->decayed;
}

/* The variable that a place is part of, when it is reached without going
   through a pointer: x for "x", "x[i]" and "x.f.g". NULL for any other. */
static const Symbol *place_root(const Expr *expr) {
    for (;;) {
        if (expr->kind == EXPR_FIELD && !expr->as.field.through_pointer) {
            expr = expr->as.field.base;
        } else if (expr->kind == EXPR_INDEX && names_array(expr->as.index.base)) {
            expr = expr->as.index.base;
        } else {
            return expr->kind == EXPR_IDENT ? expr->as.ident.symbol : NULL;
        }
    }
}

/* Reports why an expression, already checked, is not something that can be
   assigned or pointed to; returns true if it is. assigning tells which of
   the two is being done. */
static bool require_place(Checker *c, const Expr *expr, bool assigning) {
    const char *what = assigning ? "assign to" : "take the address of";
    switch (expr->kind) {
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (!symbol) return false; /* already reported */
        if (expr->as.ident.module.len) {
            error(c, expr->loc, "what a module exports can be called or read, but not "
                                "changed");
            return false;
        }
        if (symbol->kind != SYM_VAR) {
            error(c, expr->loc, "cannot %s %s '%.*s'", what, symbol_what(symbol),
                  NAME(symbol->name));
            return false;
        }
        return true;
    }
    case EXPR_INDEX: {
        const Expr *base = expr->as.index.base;
        const Symbol *root = names_array(base) ? place_root(base) : NULL;
        if (root && (root->kind == SYM_CONST || root->read_only)) {
            error(c, base->loc, "cannot %s an element of %s array '%.*s'", what,
                  root->kind == SYM_CONST ? "constant" : "read-only", NAME(root->name));
            return false;
        }
        if (names_array(base) || type_is_pointer(base->type)) return true;
        if (type_container(base->type) == CONTAINER_MAP) return true;
        const char *limit = assigning ? "changed" : "pointed to";
        if (base->type == TYPE_STRING && base->kind == EXPR_IDENT) {
            error(c, base->loc, "'%.*s' is a string, and a string's characters cannot be %s",
                  NAME(base->as.ident.name), limit);
        } else if (base->type == TYPE_STRING) {
            error(c, base->loc, "a string's characters cannot be %s", limit);
        }
        return false;
    }
    case EXPR_FIELD:
        if (expr->as.field.index < 0) return false; /* already reported */
        return expr->as.field.through_pointer ||
               require_place(c, expr->as.field.base, assigning);
    case EXPR_UNARY:
        if (expr->as.unary.op == OP_DEREF) return expr->type != TYPE_INVALID;
        break;
    default:
        break;
    }
    error(c, expr->loc, "cannot %s this: it is not a variable, an element, a field or "
                        "what a pointer points to",
          what);
    return false;
}

/* Checks an expression that names a place: the target of an assignment, or
   the operand of "&". Returns the place's type, or TYPE_INVALID with the
   problem reported if it is not one. Assigning to a variable does not count
   as using it; anything else done here does. */
static TypeKind check_place(Checker *c, Expr *expr, bool assigning) {
    const char *what = assigning ? "assign to" : "take the address of";
    if (expr->kind == EXPR_IDENT && !expr->as.ident.module.len) {
        /* A bare name is looked up here, where an array's name can be told
           what it should have been. */
        Str name = expr->as.ident.name;
        Symbol *symbol = resolve(c, name, expr->loc);
        if (!symbol) return TYPE_INVALID;
        if (!assigning) symbol->used = true;
        if (type_is_container(symbol->type) && !assigning) {
            if (type_container(symbol->type) == CONTAINER_MAP) {
                error(c, expr->loc, "cannot take the address of map '%.*s' as a whole; keep "
                                    "its keys instead",
                      NAME(name));
            } else {
                error(c, expr->loc, "cannot take the address of array '%.*s' as a whole; "
                                    "'&%.*s[0]' points to its first element",
                      NAME(name), NAME(name));
            }
            return TYPE_INVALID;
        }
        if (symbol->loop_bound && assigning) {
            error(c, expr->loc, "cannot assign to loop variable '%.*s'; it is a copy, and "
                                "changing it would change nothing it came from",
                  NAME(name));
            return TYPE_INVALID;
        }
        if (symbol->kind != SYM_VAR) {
            error(c, expr->loc, "cannot %s %s '%.*s'", what, symbol_what(symbol),
                  NAME(name));
            return TYPE_INVALID;
        }
        expr->as.ident.symbol = symbol;
        expr->type = symbol->type;
        return symbol->type;
    }

    /* The variable the place may turn out to be part of, found by name
       before the names are resolved. */
    const Expr *first = expr;
    while (first->kind == EXPR_FIELD || first->kind == EXPR_INDEX) {
        first = first->kind == EXPR_FIELD ? first->as.field.base : first->as.index.base;
    }
    Symbol *named = NULL;
    if (first->kind == EXPR_IDENT) {
        named = lookup(c, first->as.ident.module.len ? first->as.ident.module
                                                    : first->as.ident.name);
    }
    bool was_used = named && named->used;

    TypeKind type = assigning ? check_operand(c, expr) : check_expr(c, expr, TYPE_INVALID);
    if (type == TYPE_INVALID) return TYPE_INVALID;
    if (!require_place(c, expr, assigning) ||
        report_shared(c, expr, assigning ? "changed" : "pointed to")) {
        expr->type = TYPE_INVALID;
        return TYPE_INVALID;
    }
    mark_changed(expr);
    if (assigning && named && named == place_root(expr)) named->used = was_used;
    return type;
}

/* Checks assigning a whole array or map, of type: a fixed array takes a copy
   of another of its type, and a growable array a copy made with clone. */
static void check_whole_assign(Checker *c, Stmt *stmt, TypeKind type) {
    Expr *target = stmt->as.assign.target;
    Expr *value = stmt->as.assign.value;
    ContainerKind kind = type_container(type);
    Str name = target->kind == EXPR_IDENT ? target->as.ident.name : (Str){0};
    if (kind == CONTAINER_MAP || kind == CONTAINER_SLICE || stmt->as.assign.compound) {
        check_operand(c, value);
        if (kind == CONTAINER_MAP) {
            error(c, target->loc, "cannot assign to map %s as a whole; assign to its keys",
                  quoted(c, target));
        } else if (stmt->as.assign.compound) {
            error(c, stmt->loc, "operator '%s=' is not defined for '%s'",
                  stmt->as.assign.step ? (stmt->as.assign.op == OP_ADD ? "+" : "-")
                                       : op_text(stmt->as.assign.op),
                  type_name(type));
        } else {
            error(c, target->loc, "cannot assign to array %s as a whole; assign its elements",
                  quoted(c, target));
        }
        return;
    }
    if (kind == CONTAINER_LIST && !(value->kind == EXPR_CALL &&
                                    (builtin_called(c, value) == BUILTIN_CLONE ||
                                     builtin_called(c, value) == BUILTIN_POP)) &&
        value->kind != EXPR_INIT) {
        check_operand(c, value);
        if (name.len) {
            error(c, target->loc, "cannot assign to growable array '%.*s' as a whole, "
                                  "except with a copy: '%.*s = clone(other)'",
                  NAME(name), NAME(name));
        } else {
            error(c, target->loc, "cannot assign to growable array %s as a whole, except "
                                  "with a copy: '= clone(other)'",
                  quoted(c, target));
        }
        return;
    }
    TypeKind found = check_value(c, value, type);
    if (found != TYPE_INVALID && found != type && name.len) {
        error(c, value->loc, "cannot assign '%s' to '%s' variable '%.*s'", type_name(found),
              type_name(type), NAME(name));
    } else if (found != TYPE_INVALID && found != type) {
        error(c, value->loc, "cannot assign '%s' to '%s'", type_name(found), type_name(type));
    }
}

static void check_assign(Checker *c, Stmt *stmt) {
    Expr *target = stmt->as.assign.target;
    Expr *value = stmt->as.assign.value;
    TypeKind expected = check_place(c, target, true);
    if (type_is_container(expected)) {
        check_whole_assign(c, stmt, expected);
        return;
    }
    Op op = stmt->as.assign.op;
    bool compound = stmt->as.assign.compound;
    if (compound && op_is_shift(op)) {
        check_shift_amount(c, stmt->as.assign.value);
        if (expected != TYPE_INVALID && !type_is_integer(expected)) {
            error(c, stmt->loc, "operator '%s=' is not defined for '%s'", op_text(op),
                  type_name(expected));
        }
        return;
    }
    /* With no place to say which pointer type it is, a NULL has nothing
       more to report than the place's own problem. */
    if (expected == TYPE_INVALID && stmt->as.assign.value->kind == EXPR_NULL) return;
    if (expected == TYPE_INVALID) {
        check_value(c, value, TYPE_INVALID);
        return;
    }
    TypeKind found = check_expr(c, value, expected);
    if (compound && op == OP_ADD && expected == TYPE_STRING && !stmt->as.assign.step) {
        /* "s += v" joins v to the end of s. */
        if (found == TYPE_VOID) {
            error(c, stmt->as.assign.value->loc, "cannot join a 'void' value to a string");
        }
        return;
    }
    if (type_owns(expected) && !compound && !is_fresh(stmt->as.assign.value)) {
        report_copy(c, stmt->as.assign.value->loc, expected,
                    "change its fields, or point to it with a pointer");
        return;
    }

    if (compound && type_is_pointer(expected) && (op == OP_ADD || op == OP_SUB)) {
        /* "p += n" moves the pointer by n elements. */
        if (found != TYPE_INVALID && !is_offset(found)) {
            error(c, stmt->as.assign.value->loc,
                  "a pointer moves by an integer of at most 64 bits, found '%s'",
                  type_name(found));
        }
        return;
    }
    bool needs_integer = op == OP_MOD || is_bitwise(op);
    if (compound && stmt->as.assign.step && !type_is_numeric(expected)) {
        error(c, stmt->loc, "operator '%s' is not defined for '%s'", op == OP_ADD ? "++" : "--",
              type_name(expected));
        return;
    }
    if (compound && !(needs_integer ? type_is_integer(expected) : type_is_numeric(expected))) {
        error(c, stmt->loc, "operator '%s=' is not defined for '%s'", op_text(op),
              type_name(expected));
    } else if (found != TYPE_INVALID && found != expected) {
        /* Says what kind of place it is when a variable's name can be given
           with it. */
        const Symbol *root = place_root(target);
        if (root) {
            error(c, stmt->as.assign.value->loc, "cannot assign '%s' to '%s' %s '%.*s'",
                  type_name(found), type_name(expected),
                  target->kind == EXPR_FIELD   ? "field of"
                  : target->kind == EXPR_INDEX ? "element of"
                                               : "variable",
                  NAME(root->name));
        } else {
            error(c, stmt->as.assign.value->loc, "cannot assign '%s' to '%s'",
                  type_name(found), type_name(expected));
        }
    }
}

static void check_return(Checker *c, Stmt *stmt) {
    Expr *value = stmt->as.ret.value;
    if (c->in_defer) {
        error(c, stmt->loc, "a deferred statement cannot 'return'; it runs as its block is "
                            "left");
    }
    if (!c->func) {
        error(c, stmt->loc, "'return' outside of a function");
        if (value) check_expr(c, value, TYPE_INVALID);
        return;
    }
    TypeKind expected = c->func->as.func.ret;
    Str name = c->func->as.func.name;
    if (!value) {
        if (expected != TYPE_VOID) {
            error(c, stmt->loc, "'%.*s' must return a '%s' value", NAME(name),
                  type_name(expected));
        }
        return;
    }
    TypeKind found = check_expr(c, value, expected);
    /* A pointer into the function's own variables outlives them. */
    const Symbol *local = NULL;
    if (value->kind == EXPR_UNARY && value->as.unary.op == OP_ADDR) {
        local = place_root(value->as.unary.operand);
    } else if (value->decayed) {
        local = place_root(value);
    }
    if (local && local->kind == SYM_VAR && !local->global &&
        type_container(local->type) != CONTAINER_SLICE) {
        diag_add(c->diags, SEV_WARNING, value->loc,
                 "this returns a pointer to '%.*s', which is gone once '%.*s' returns",
                 NAME(local->name), NAME(name));
    }
    if (expected == TYPE_VOID) {
        error(c, value->loc, "'%.*s' returns 'void' and cannot return a value",
              NAME(name));
    } else if (found != TYPE_INVALID && found != expected) {
        error(c, value->loc, "'%.*s' returns '%s', found '%s'", NAME(name),
              type_name(expected), type_name(found));
    }
}

static void check_stmt(Checker *c, Stmt *stmt);
static void warn_after_exit(Checker *c, const Block *block);

/* Checks a block's statements in a scope of their own. */
static void check_block(Checker *c, Block *block) {
    Scope scope;
    push_scope(c, &scope, block->close.offset);
    for (size_t i = 0; i < block->count; i++) check_stmt(c, block->stmts[i]);
    pop_scope(c);
    warn_after_exit(c, block);
}

/* Labels are declared up front so that a goto can jump forward. They belong
   to the whole function, whatever block they sit in. */
static void collect_labels(Checker *c, Block *block) {
    for (size_t i = 0; i < block->count; i++) {
        Stmt *stmt = block->stmts[i];
        if (stmt->kind == STMT_BLOCK) {
            collect_labels(c, &stmt->as.block);
        } else if (stmt->kind == STMT_IF) {
            collect_labels(c, &stmt->as.if_.then_block);
            if (stmt->as.if_.has_else) collect_labels(c, &stmt->as.if_.else_block);
        } else if (stmt->kind == STMT_WHILE || stmt->kind == STMT_DO_WHILE ||
                   stmt->kind == STMT_FOR || stmt->kind == STMT_FOR_IN) {
            collect_labels(c, &stmt->as.loop.body);
        } else if (stmt->kind == STMT_SWITCH) {
            for (size_t j = 0; j < stmt->as.switch_.case_count; j++) {
                collect_labels(c, &stmt->as.switch_.cases[j].body);
            }
        } else if (stmt->kind == STMT_LABEL) {
            Str name = stmt->as.label.name;
            const Symbol *prev = find_in(&c->labels, name);
            if (prev) {
                error(c, stmt->loc, "label '%.*s' is already defined at line %d",
                      NAME(name), prev->loc.line);
                continue;
            }
            Symbol *symbol = new_symbol(c, SYM_LABEL, name, stmt->loc, TYPE_INVALID);
            stmt->as.label.symbol = symbol;
            list_push(c, &c->labels, symbol);
        }
    }
}

/* True if the block holds a break that would leave the loop it is the body
   of: one not inside a loop nested within it. */
static bool breaks_out(const Block *block) {
    for (size_t i = 0; i < block->count; i++) {
        const Stmt *stmt = block->stmts[i];
        if (stmt->kind == STMT_BREAK) return true;
        if (stmt->kind == STMT_BLOCK && breaks_out(&stmt->as.block)) return true;
        if (stmt->kind == STMT_IF &&
            (breaks_out(&stmt->as.if_.then_block) ||
             (stmt->as.if_.has_else && breaks_out(&stmt->as.if_.else_block)))) {
            return true;
        }
    }
    return false;
}

/* True if a loop can only be left by a return or goto: it has no condition
   (or the literal true) and no break. */
static bool loops_forever(const Stmt *loop) {
    const Expr *cond = loop->as.loop.cond;
    bool always = !cond || (cond->kind == EXPR_BOOL_LIT && cond->as.bool_lit);
    return always && !breaks_out(&loop->as.loop.body);
}

static bool stmt_has_label(const Stmt *stmt);
static bool switch_terminates(const Stmt *stmt);

/* True for a statement that is a call of exit, which ends the program. */
static bool is_exit(const Stmt *stmt) {
    return stmt->kind == STMT_EXPR && stmt->as.expr->kind == EXPR_CALL &&
           (stmt->as.expr->as.call.builtin == BUILTIN_EXIT ||
            stmt->as.expr->as.call.builtin == BUILTIN_PANIC);
}

/* True if control cannot run off the end of the block. Conservative: it only
   recognizes a return, a goto or an exit, an if/else whose branches both
   qualify, and a loop that never ends. Whatever follows such a statement
   cannot be reached, unless a label lets a goto back in. */
static bool block_terminates(const Block *block) {
    bool exits = false;
    for (size_t i = 0; i < block->count; i++) {
        const Stmt *stmt = block->stmts[i];
        if (stmt_has_label(stmt)) exits = false;
        if (is_exit(stmt)) exits = true;
        switch (stmt->kind) {
        case STMT_RETURN:
        case STMT_GOTO:
            exits = true;
            break;
        case STMT_BLOCK:
            if (block_terminates(&stmt->as.block)) exits = true;
            break;
        case STMT_IF:
            if (stmt->as.if_.has_else && block_terminates(&stmt->as.if_.then_block) &&
                block_terminates(&stmt->as.if_.else_block)) {
                exits = true;
            }
            break;
        case STMT_WHILE:
        case STMT_DO_WHILE:
        case STMT_FOR:
            if (loops_forever(stmt)) exits = true;
            break;
        case STMT_SWITCH:
            if (switch_terminates(stmt)) exits = true;
            break;
        default:
            break;
        }
    }
    return exits;
}

/* True if a break that leaves target is anywhere in the block. */
static bool block_breaks_to(const Block *block, const Stmt *target) {
    for (size_t i = 0; i < block->count; i++) {
        const Stmt *stmt = block->stmts[i];
        switch (stmt->kind) {
        case STMT_BREAK:
            if (stmt->as.target == target) return true;
            break;
        case STMT_BLOCK:
            if (block_breaks_to(&stmt->as.block, target)) return true;
            break;
        case STMT_IF:
            if (block_breaks_to(&stmt->as.if_.then_block, target) ||
                (stmt->as.if_.has_else && block_breaks_to(&stmt->as.if_.else_block, target))) {
                return true;
            }
            break;
        case STMT_WHILE:
        case STMT_DO_WHILE:
        case STMT_FOR:
        case STMT_FOR_IN:
            if (block_breaks_to(&stmt->as.loop.body, target)) return true;
            break;
        case STMT_SWITCH:
            for (size_t j = 0; j < stmt->as.switch_.case_count; j++) {
                if (block_breaks_to(&stmt->as.switch_.cases[j].body, target)) return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

/* True if no value leaves the switch to what follows it: it has a default,
   nothing breaks out of it, and every case leaves the block or falls into
   the next. */
static bool switch_terminates(const Stmt *stmt) {
    bool has_default = false;
    for (size_t i = 0; i < stmt->as.switch_.case_count; i++) {
        const SwitchCase *each = &stmt->as.switch_.cases[i];
        if (each->is_default) has_default = true;
        if (block_breaks_to(&each->body, stmt)) return false;
        if (!each->falls && !block_terminates(&each->body)) return false;
    }
    return has_default;
}

/* A goto could jump past a defer, or out of the block it belongs to, so a
   function, or the top-level code, holds one or the other. */
static void check_goto_with_defer(Checker *c) {
    if (c->goto_seen && c->defer_seen) {
        error(c, c->goto_seen->loc, "'goto' cannot be used where there is a 'defer', which "
                                    "it could jump past");
    }
}

static void check_func(Checker *c, Stmt *stmt) {
    if (stmt->external) return; /* its signature was checked with the others */
    Stmt *outer_func = c->func;
    SymbolList *outer_locals = c->locals;
    SymbolList outer_labels = c->labels;
    Scope *outer_scope = c->scope;
    const Stmt *outer_loop = c->loop;
    const Stmt *outer_breakable = c->breakable;

    /* A function body sees the top level only. */
    c->scope = c->global;
    c->func = stmt;
    c->loop = NULL;
    c->breakable = NULL;
    c->locals = &stmt->as.func.locals;
    memset(&c->labels, 0, sizeof c->labels);

    Block *body = &stmt->as.func.body;
    Scope scope;
    push_scope(c, &scope, body->close.offset);
    for (size_t i = 0; i < stmt->as.func.param_count; i++) {
        Param *param = &stmt->as.func.params[i];
        Symbol *symbol = new_symbol(c, SYM_PARAM, param->name, param->loc, param->type);
        symbol->read_only = type_container(param->type) == CONTAINER_SLICE && param->is_const;
        symbol->shares = !type_is_container(param->type) && type_owns(param->type);
        symbol->visible_from = body->open.offset;
        param->symbol = symbol;
        declare(c, symbol);
    }
    const Stmt *outer_goto = c->goto_seen;
    const Stmt *outer_defer = c->defer_seen;
    c->goto_seen = c->defer_seen = NULL;
    collect_labels(c, body);
    for (size_t i = 0; i < body->count; i++) check_stmt(c, body->stmts[i]);
    check_goto_with_defer(c);
    c->goto_seen = outer_goto;
    c->defer_seen = outer_defer;
    warn_after_exit(c, body);
    pop_scope(c);

    if (stmt->as.func.ret != TYPE_VOID && !block_terminates(body)) {
        error(c, stmt->loc, "'%.*s' can reach its end without returning a '%s' value",
              NAME(stmt->as.func.name), type_name(stmt->as.func.ret));
    }

    c->func = outer_func;
    c->locals = outer_locals;
    c->labels = outer_labels;
    c->loop = outer_loop;
    c->breakable = outer_breakable;
    c->scope = outer_scope;
}

/* ---- Unreachable code ---- */

static bool block_has_label(const Block *block);

static bool stmt_has_label(const Stmt *stmt) {
    switch (stmt->kind) {
    case STMT_LABEL:
        return true;
    case STMT_BLOCK:
        return block_has_label(&stmt->as.block);
    case STMT_IF:
        return block_has_label(&stmt->as.if_.then_block) ||
               (stmt->as.if_.has_else && block_has_label(&stmt->as.if_.else_block));
    case STMT_WHILE:
    case STMT_DO_WHILE:
    case STMT_FOR:
    case STMT_FOR_IN:
        return block_has_label(&stmt->as.loop.body);
    case STMT_SWITCH:
        for (size_t i = 0; i < stmt->as.switch_.case_count; i++) {
            if (block_has_label(&stmt->as.switch_.cases[i].body)) return true;
        }
        return false;
    default:
        return false;
    }
}

static bool block_has_label(const Block *block) {
    for (size_t i = 0; i < block->count; i++) {
        if (stmt_has_label(block->stmts[i])) return true;
    }
    return false;
}

/* Reports the statements from first to last as code that cannot run. */
static void warn_unreachable(Checker *c, const Stmt *first, const Stmt *last,
                             const char *why) {
    diag_add(c->diags, SEV_WARNING, first->start, "unreachable code: %s", why);
    diag_mark_unnecessary(c->diags, last->end);
}

/* Reports a whole block that its guarding condition rules out. */
static void warn_dead_block(Checker *c, const Block *block, const char *why) {
    if (block->count == 0 || block_has_label(block)) return;
    warn_unreachable(c, block->stmts[0], block->stmts[block->count - 1], why);
}

static bool is_literal(const Expr *cond, bool value) {
    return cond && cond->kind == EXPR_BOOL_LIT && cond->as.bool_lit == value;
}

/* If control never passes from this statement to the next, returns a
   description of why; otherwise NULL. */
static const char *never_continues(const Stmt *stmt) {
    switch (stmt->kind) {
    case STMT_RETURN:   return "it follows a 'return'";
    /* A misplaced break or continue is an error already, and says nothing
       about what follows it. */
    case STMT_BREAK:    return stmt->as.target ? "it follows a 'break'" : NULL;
    case STMT_CONTINUE: return stmt->as.target ? "it follows a 'continue'" : NULL;
    case STMT_GOTO:     return "it follows a 'goto'";
    case STMT_SWITCH:
        return switch_terminates(stmt) ? "every case of the 'switch' before it leaves this block"
                                       : NULL;
    case STMT_EXPR:
        return !is_exit(stmt)                                         ? NULL
               : stmt->as.expr->as.call.builtin == BUILTIN_EXIT ? "it follows a call of 'exit'"
                                                                : "it follows a call of 'panic'";
    case STMT_BLOCK:
        return block_terminates(&stmt->as.block) ? "the block before it never reaches its end"
                                                 : NULL;
    case STMT_WHILE:
    case STMT_DO_WHILE:
    case STMT_FOR:
        return loops_forever(stmt) ? "it follows a loop that never ends" : NULL;
    case STMT_IF:
        return stmt->as.if_.has_else && block_terminates(&stmt->as.if_.then_block) &&
                       block_terminates(&stmt->as.if_.else_block)
                   ? "every branch of the 'if' before it leaves this block"
                   : NULL;
    default:
        return NULL;
    }
}

/* Reports what follows the first statement of a block that never continues,
   up to the next label. Function declarations are not code that runs, so a
   region is reported in the pieces between them. */
static void warn_after_exit(Checker *c, const Block *block) {
    for (size_t i = 0; i + 1 < block->count; i++) {
        const char *why = never_continues(block->stmts[i]);
        if (!why) continue;

        size_t end = i + 1;
        while (end < block->count && block->stmts[end]->kind != STMT_LABEL) {
            if (stmt_has_label(block->stmts[end])) return;
            end++;
        }
        const Stmt *first = NULL;
        for (size_t k = i + 1; k <= end; k++) {
            bool is_code = k < end && block->stmts[k]->kind != STMT_FUNC;
            if (is_code && !first) first = block->stmts[k];
            if (!is_code && first) {
                warn_unreachable(c, first, block->stmts[k - 1], why);
                first = NULL;
            }
        }
        return;
    }
}

static void check_condition(Checker *c, Expr *cond) {
    TypeKind type = check_expr(c, cond, TYPE_INVALID);
    if (!is_truth(type) && type != TYPE_INVALID) {
        error(c, cond->loc, "condition must be 'bool' or a pointer, found '%s'",
              type_name(type));
    }
}

/* The text of a string constant: a literal, or a constant set to one. */
static bool const_text(const Expr *expr, Str *out) {
    if (expr->kind == EXPR_STRING_LIT) {
        *out = expr->as.string_lit;
        return true;
    }
    if (expr->kind != EXPR_IDENT) return false;
    const Symbol *symbol = expr->as.ident.symbol;
    if (!symbol || symbol->kind != SYM_CONST || type_is_container(symbol->type) || !symbol->decl ||
        symbol->decl->external || symbol->decl->as.decl.broken || !symbol->decl->as.decl.init) {
        return false;
    }
    return const_text(symbol->decl->as.decl.init, out);
}

/* True if a case's statements never run on to the end of the case: the
   last of them leaves, by a break, a continue, a fallthrough or a way out
   of the block. */
static bool case_leaves(const Block *body) {
    if (body->count == 0) return false;
    if (block_terminates(body)) return true;
    const Stmt *last = body->stmts[body->count - 1];
    switch (last->kind) {
    case STMT_BREAK:
    case STMT_CONTINUE:
    case STMT_FALLTHROUGH:
        return true;
    case STMT_BLOCK:
        return case_leaves(&last->as.block);
    case STMT_IF:
        return last->as.if_.has_else && case_leaves(&last->as.if_.then_block) &&
               case_leaves(&last->as.if_.else_block);
    default:
        return false;
    }
}

/* Checks "switch (value) { ... }": the value is an integer, a char, an
   enum or a string, and each case's values are constants of its type that
   no other case has. A case that has statements must not run on into the
   next unless it says so with "fallthrough", since C's silent fall is a
   common mistake. */
static void check_switch(Checker *c, Stmt *stmt) {
    Expr *subject = stmt->as.switch_.subject;
    TypeKind type = check_expr(c, subject, TYPE_INVALID);
    bool is_string = type == TYPE_STRING;
    bool usable = is_string || type_is_integer(type) || type == TYPE_CHAR || type_is_enum(type);
    if (type != TYPE_INVALID && !usable) {
        error(c, subject->loc,
              "a switch is on an integer, a 'char', an enum or a string, not '%s'",
              type_name(type));
    }
    const SwitchCase *default_case = NULL;
    size_t count = stmt->as.switch_.case_count;
    for (size_t i = 0; i < count; i++) {
        SwitchCase *each = &stmt->as.switch_.cases[i];
        if (each->is_default && default_case) {
            error(c, each->loc, "a switch has one 'default', and this one has one at line %d",
                  default_case->loc.line);
        } else if (each->is_default) {
            default_case = each;
        }
        if (!is_string && each->value_count) {
            each->numbers = arena_alloc(c->arena, each->value_count * sizeof *each->numbers);
        }
        for (size_t j = 0; j < each->value_count; j++) {
            Expr *value = each->values[j];
            TypeKind found = check_expr(c, value, usable ? type : TYPE_INVALID);
            if (!usable || found == TYPE_INVALID) continue;
            /* A value that is wrong is marked so, and matches no other. */
            if (found != type) {
                error(c, value->loc, "this case is a '%s', and the switch is on '%s'",
                      type_name(found), type_name(type));
                value->type = TYPE_INVALID;
                continue;
            }
            if (!is_constant(value)) {
                error(c, value->loc, "a case must be a value known at compile time");
                value->type = TYPE_INVALID;
                continue;
            }
            /* The same value in an earlier case, or earlier in this one. */
            Str text = {0};
            long long number = 0;
            if (is_string ? !const_text(value, &text) : !eval_const_int(value, &number)) {
                error(c, value->loc, "a case must be a literal, or a constant set to one");
                value->type = TYPE_INVALID;
                continue;
            }
            if (!is_string) each->numbers[j] = number;
            for (size_t k = 0; k <= i; k++) {
                const SwitchCase *other = &stmt->as.switch_.cases[k];
                size_t limit = k == i ? j : other->value_count;
                for (size_t m = 0; m < limit; m++) {
                    Str other_text;
                    bool same = is_string ? const_text(other->values[m], &other_text) &&
                                                str_eq(text, other_text)
                                          : other->numbers && other->numbers[m] == number &&
                                                other->values[m]->type == type;
                    if (same) {
                        error(c, value->loc, "this value already has a case, at line %d",
                              other->values[m]->loc.line);
                        k = i + 1;
                        break;
                    }
                }
            }
        }

        const Stmt *outer_breakable = c->breakable;
        const Stmt *outer_fallthrough = c->fallthrough_ok;
        Block *body = &each->body;
        const Stmt *last = body->count ? body->stmts[body->count - 1] : NULL;
        each->falls = last && last->kind == STMT_FALLTHROUGH;
        c->breakable = stmt;
        c->fallthrough_ok = each->falls ? last : NULL;
        check_block(c, body);
        c->breakable = outer_breakable;
        c->fallthrough_ok = outer_fallthrough;
        if (each->falls && i + 1 == count) {
            error(c, last->loc, "'fallthrough' needs a case after it to go on to");
        } else if (i + 1 < count && body->count > 0 && !case_leaves(body)) {
            error(c, each->loc,
                  "this case runs on into the next; end it with 'break', or with "
                  "'fallthrough' if that is meant");
        }
    }
}

static void check_loop(Checker *c, Stmt *stmt) {
    const Stmt *outer_loop = c->loop;
    Block *body = &stmt->as.loop.body;
    stmt->as.loop.id = c->next_id++;

    /* A variable declared in a for loop's first clause belongs to the loop. */
    Scope scope;
    push_scope(c, &scope, body->close.offset);
    if (stmt->as.loop.init) check_stmt(c, stmt->as.loop.init);
    if (stmt->kind != STMT_DO_WHILE && stmt->as.loop.cond) {
        check_condition(c, stmt->as.loop.cond);
    }
    if (stmt->as.loop.step) check_stmt(c, stmt->as.loop.step);

    const Stmt *outer_breakable = c->breakable;
    c->loop = c->breakable = stmt;
    check_block(c, body);
    c->loop = outer_loop;
    c->breakable = outer_breakable;
    /* A do-while runs its body once whatever the condition says. */
    if (stmt->kind != STMT_DO_WHILE && is_literal(stmt->as.loop.cond, false)) {
        warn_dead_block(c, body, "the loop's condition is always false");
    }

    /* A do-while's condition is outside the body's scope, as in C. */
    if (stmt->kind == STMT_DO_WHILE) check_condition(c, stmt->as.loop.cond);
    pop_scope(c);
}

/* Checks "for (T x in over)" and its two-variable forms. Over an array or a
   string, one variable takes each element and two take its index and the
   element; over a map, they take each key, and its value. */
static void check_for_in(Checker *c, Stmt *stmt) {
    const Stmt *outer_loop = c->loop;
    Block *body = &stmt->as.loop.body;
    stmt->as.loop.id = c->next_id++;
    Expr *over = stmt->as.loop.over;
    size_t count = stmt->as.loop.each_count;

    /* What each loop variable takes, and what that is called. */
    TypeKind takes[2] = {TYPE_INVALID, TYPE_INVALID};
    const char *what[2] = {"", ""};
    TypeKind found = check_operand(c, over);
    bool map = type_container(found) == CONTAINER_MAP;
    bool array = any_array(found);
    TypeKind element = array ? type_elem(found) : found == TYPE_STRING ? TYPE_CHAR : TYPE_INVALID;
    if (map) {
        takes[0] = type_key(found);
        takes[1] = type_elem(found);
        what[0] = "each key";
        what[1] = "each value";
    } else if (element != TYPE_INVALID) {
        takes[count - 1] = element;
        what[count - 1] = array ? "each element" : "each character";
        if (count == 2) {
            takes[0] = TYPE_INT;
            what[0] = "the index";
        }
    } else if (found != TYPE_INVALID && element == TYPE_INVALID) {
        error(c, over->loc, "'for ... in' goes through an array, a map or a string, found "
                            "'%s'",
              type_name(found));
    }

    Scope scope;
    push_scope(c, &scope, body->close.offset);
    for (size_t i = 0; i < count; i++) {
        Param *each = &stmt->as.loop.each[i];
        each->type = declared_type(c, each->type, each->type_ref, each->loc, each->name);
        each->type = apply_suffixes(c, each->type, each->suffixes, each->suffix_count,
                                    SUFFIXES_EACH, 0, each->name);
        if (takes[i] != TYPE_INVALID && each->type != TYPE_INVALID && each->type != takes[i]) {
            error(c, each->loc, "'%.*s' takes %s, which is '%s', not '%s'", NAME(each->name),
                  what[i], type_name(takes[i]), type_name(each->type));
        }
        Symbol *symbol = new_symbol(c, SYM_VAR, each->name, each->loc, each->type);
        symbol->loop_bound = true;
        symbol->shares = type_owns(each->type);
        symbol->visible_from = body->open.offset;
        each->symbol = symbol;
        declare(c, symbol);
        list_push(c, c->locals, symbol);
    }
    const Stmt *outer_breakable = c->breakable;
    c->loop = c->breakable = stmt;
    check_block(c, body);
    c->loop = outer_loop;
    c->breakable = outer_breakable;
    pop_scope(c);
}

/* Settles what a read into a variable, or one of its elements, stores: the
   place its ".field" steps lead to, which must be of a type that input can
   be converted to. */
static void accept_read_target(Checker *c, ReadTarget *target, const Symbol *symbol,
                               TypeKind read) {
    TypeKind type = check_place_fields(c, read, target->fields, target->field_count);
    if (type_is_container(type)) {
        error(c, target->loc, "cannot read into an array or a map as a whole; read its "
                              "elements");
        return;
    }
    if (type_info(type)) {
        error(c, target->loc, "a '%s' cannot be read from input%s", type_name(type),
              type_is_struct(type) ? "; read its fields" : "");
        return;
    }
    if (type == TYPE_INVALID && target->field_count) return;
    target->type = type;
    target->symbol = symbol;
}

static void check_stmt(Checker *c, Stmt *stmt) {
    switch (stmt->kind) {
    case STMT_WHILE:
    case STMT_DO_WHILE:
    case STMT_FOR:
        check_loop(c, stmt);
        break;
    case STMT_FOR_IN:
        check_for_in(c, stmt);
        break;
    case STMT_SCRATCH:
        break; /* made by the parser for #temp, and right as it is */
    case STMT_DEFER: {
        if (!c->defer_seen) c->defer_seen = stmt;
        const Stmt *outer_loop = c->loop;
        const Stmt *outer_breakable = c->breakable;
        bool outer_in_defer = c->in_defer;
        c->loop = c->breakable = NULL;
        c->in_defer = true;
        check_stmt(c, stmt->as.deferred);
        c->loop = outer_loop;
        c->breakable = outer_breakable;
        c->in_defer = outer_in_defer;
        break;
    }
    case STMT_BREAK:
    case STMT_CONTINUE:
        stmt->as.target = stmt->kind == STMT_BREAK ? c->breakable : c->loop;
        if (!stmt->as.target && c->in_defer) {
            error(c, stmt->loc, "a deferred statement cannot leave with '%s'; it runs as "
                                "its block is left",
                  stmt->kind == STMT_BREAK ? "break" : "continue");
        } else if (!stmt->as.target) {
            error(c, stmt->loc, "'%s' outside of a loop%s",
                  stmt->kind == STMT_BREAK ? "break" : "continue",
                  stmt->kind == STMT_BREAK ? " or a switch" : "");
        }
        break;
    case STMT_SWITCH:
        check_switch(c, stmt);
        break;
    case STMT_FALLTHROUGH:
        if (stmt != c->fallthrough_ok) {
            error(c, stmt->loc, "'fallthrough' can only be the last statement of a case "
                                "of a switch");
        }
        break;
    case STMT_CONST:
    case STMT_VAR:
        check_decl(c, stmt);
        break;
    case STMT_ASSIGN:
        check_assign(c, stmt);
        break;
    case STMT_EXPR:
        check_expr(c, stmt->as.expr, TYPE_INVALID);
        break;
    case STMT_COUT:
        for (size_t i = 0; i < stmt->as.cout.count; i++) {
            Expr *arg = stmt->as.cout.args[i];
            check_written(c, arg, "print");
        }
        break;
    case STMT_CIN:
        for (size_t i = 0; i < stmt->as.cin.count; i++) {
            ReadTarget *target = &stmt->as.cin.targets[i];
            if (target->declares) {
                target->type = declared_type(c, target->type, target->type_ref,
                                             target->loc, target->name);
                if (type_info(target->type)) {
                    error(c, target->loc, "a '%s' cannot be read from input",
                          type_name(target->type));
                    target->type = TYPE_INVALID;
                }
                Symbol *declared =
                    new_symbol(c, SYM_VAR, target->name, target->loc, target->type);
                declare(c, declared);
                list_push(c, declared->global ? &c->unit->program.globals : c->locals,
                          declared);
                target->symbol = declared;
                continue;
            }
            Symbol *symbol = resolve(c, target->name, target->loc);
            if (!symbol) {
                if (target->index) check_expr(c, target->index, TYPE_INT);
                continue;
            }
            bool is_array = any_array(symbol->type);
            if (type_container(symbol->type) == CONTAINER_MAP) {
                if (target->index) check_expr(c, target->index, type_key(symbol->type));
                error(c, target->loc, "cannot read into map '%.*s'; read into a variable, "
                                      "then store it under its key",
                      NAME(target->name));
                continue;
            }
            if (target->index) {
                Indexed of = {.what = "an array",
                              .length_known = type_container(symbol->type) == CONTAINER_ARRAY,
                              .length = type_length(symbol->type), .name = target->name,
                              .runtime_length = "an array parameter's length is known only "
                                                "when the program runs"};
                check_index_value(c, target->index, of);
                if (!is_array) {
                    error(c, target->loc, "'%.*s' is not an array", NAME(target->name));
                } else if (symbol->kind == SYM_CONST || symbol->read_only) {
                    error(c, target->loc, "cannot read into an element of %s array '%.*s'",
                          symbol->kind == SYM_CONST ? "constant" : "read-only",
                          NAME(target->name));
                } else {
                    accept_read_target(c, target, symbol, type_elem(symbol->type));
                }
                continue;
            }
            if (is_array) {
                error(c, target->loc,
                      "cannot read into array '%.*s' as a whole; read its elements",
                      NAME(target->name));
                continue;
            }
            if (symbol->kind != SYM_VAR) {
                error(c, target->loc, "cannot read into %s '%.*s'", symbol_what(symbol),
                      NAME(target->name));
                continue;
            }
            accept_read_target(c, target, symbol, symbol->type);
        }
        break;
    case STMT_IF:
        check_condition(c, stmt->as.if_.cond);
        check_block(c, &stmt->as.if_.then_block);
        if (stmt->as.if_.has_else) check_block(c, &stmt->as.if_.else_block);
        if (is_literal(stmt->as.if_.cond, false)) {
            warn_dead_block(c, &stmt->as.if_.then_block, "the condition is always false");
        } else if (is_literal(stmt->as.if_.cond, true) && stmt->as.if_.has_else) {
            warn_dead_block(c, &stmt->as.if_.else_block, "the condition is always true");
        }
        break;
    case STMT_GOTO: {
        if (!c->goto_seen) c->goto_seen = stmt;
        if (c->in_defer) {
            error(c, stmt->loc, "a deferred statement cannot leave with 'goto'");
        }
        Str name = stmt->as.label.name;
        Symbol *symbol = find_in(&c->labels, name);
        if (!symbol) {
            error(c, stmt->loc, "undefined label '%.*s'", NAME(name));
            break;
        }
        symbol->used = true;
        stmt->as.label.symbol = symbol;
        add_ref(c, stmt->loc, symbol);
        break;
    }
    case STMT_LABEL:
        break;
    case STMT_RETURN:
        check_return(c, stmt);
        break;
    case STMT_FUNC:
        check_func(c, stmt);
        break;
    case STMT_BLOCK:
        check_block(c, &stmt->as.block);
        break;
    case STMT_IMPORT:
    case STMT_TYPE:
        break; /* handled before anything else is checked */
    case STMT_GROUP:
        /* Left only in a for loop's clauses: blocks hold a group's
           declarations in its place. */
        for (size_t i = 0; i < stmt->as.block.count; i++) check_stmt(c, stmt->as.block.stmts[i]);
        break;
    case STMT_SETTING:
        break; /* see check_settings */
    }
}

static void warn_unused(Checker *c) {
    /* A statement with an error in it is checked no further, and what it
       uses goes uncounted. */
    if (c->diags->error_count) return;
    for (size_t i = 0; i < c->unit->symbols.count; i++) {
        const Symbol *symbol = c->unit->symbols.items[i];
        /* A loop variable may be named only to reach the one after it. */
        if (symbol->kind == SYM_PARAM || symbol->kind == SYM_TYPE ||
            symbol->kind == SYM_ENUM_VALUE || symbol->loop_bound) {
            continue;
        }
        /* main is called by the runtime, and what a file exports is there
           for the files that import it. */
        if (symbol->decl && symbol->decl == c->unit->program.main) continue;
        if (symbol->exported) continue;
        /* A function named loop is called by a platform that repeats it,
           and is left alone on the others so that a program for such a
           platform checks cleanly anywhere. */
        if (symbol->kind == SYM_FUNC && symbol->global && str_eq_cstr(symbol->name, "loop")) {
            continue;
        }
        if (!symbol->used) {
            diag_add(c->diags, SEV_WARNING, symbol->loc, "%s '%.*s' is never used",
                     symbol_what(symbol), NAME(symbol->name));
        }
    }
}

static bool is_declaration(const Stmt *stmt) {
    return stmt->kind == STMT_CONST || stmt->kind == STMT_VAR ||
           stmt->kind == STMT_FUNC || stmt->kind == STMT_IMPORT ||
           stmt->kind == STMT_TYPE || stmt->kind == STMT_SETTING;
}

/* Checks the settings at the top of a file. */
static void check_settings(Checker *c) {
    const Block *top = &c->unit->program.top;
    bool is_program = c->unit->root == c->unit;
    const char *building = types_platform()->name;
    const Stmt *seen_target = NULL, *seen_board = NULL, *seen_module = NULL;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *stmt = top->stmts[i];
        if (stmt->kind != STMT_SETTING) continue;
        Str name = stmt->as.setting.name;
        Str value = stmt->as.setting.value;
        bool is_target = str_eq_cstr(name, "target");
        bool is_module = str_eq_cstr(name, "module");
        const Stmt **seen = is_target ? &seen_target : is_module ? &seen_module : &seen_board;
        if (!stmt->as.setting.leading) {
            error(c, stmt->loc, "'#%.*s' goes at the top of the file, above everything but "
                                "other settings",
                  NAME(name));
        } else if (*seen) {
            error(c, stmt->loc, "'#%.*s' is already given at line %d", NAME(name),
                  (*seen)->loc.line);
        } else {
            *seen = stmt;
        }
        if (*seen != stmt) continue;
        if (is_module) {
            c->unit->module_name = value;
            continue;
        }
        if (is_target && !target_find(value)) {
            error(c, stmt->as.setting.value_loc,
                  "unknown target '%.*s'; the targets are " TARGET_NAMES, NAME(value));
        } else if (is_target && !is_program && !str_eq_cstr(value, building)) {
            /* A file written for one target, imported by a program for
               another. */
            error(c, stmt->as.setting.value_loc,
                  "this file is for target '%.*s', and the program is being built for "
                  "'%s'; give the program '#target %.*s'",
                  NAME(value), building, NAME(value));
        } else if (!is_target && !is_program) {
            error(c, stmt->loc, "'#board' belongs in the program's own file, not in one it "
                                "imports");
        } else if (!is_target && !target_find((Str){building, strlen(building)})->run) {
            error(c, stmt->loc, "'#board' names a board, which a program for %s does not run "
                                "on",
                  building);
        } else if (!is_target && value.len == 0) {
            error(c, stmt->as.setting.value_loc, "'#board' needs the board's name");
        }
    }
}

/* A file that defines main is laid out like a C program: declarations at the
   top level, and main as the entry point. */
static void check_main(Checker *c, const Stmt *main) {
    c->unit->program.main = main;
    const Param *params = main->as.func.params;
    bool takes_args = main->as.func.param_count == 2 && params[0].type == TYPE_INT &&
                      params[1].type == type_slice_of(TYPE_STRING);
    if (main->as.func.param_count != 0 && !takes_args) {
        error(c, main->loc,
              "'main' takes no parameters, or the program's arguments as "
              "'(int argc, string argv[])'");
    } else if (takes_args && !types_platform()->has_arguments) {
        error(c, main->loc, "a program for %s has no arguments; 'main' takes no parameters",
              types_platform()->name);
    }
    if (main->external) error(c, main->loc, "'main' cannot be external");
    if (main->as.func.ret != TYPE_INT && main->as.func.ret != TYPE_VOID) {
        error(c, main->loc, "'main' must return 'int' or 'void', not '%s'",
              type_name(main->as.func.ret));
    }
    const Block *top = &c->unit->program.top;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *stmt = top->stmts[i];
        if (!is_declaration(stmt)) {
            error(c, stmt->loc,
                  "a file that defines 'main' can only have declarations at the top "
                  "level; move this statement into a function");
        }
    }
}

/* ---- Type declarations ---- */

static Symbol *declare_type_name(Checker *c, Stmt *stmt, Str name, SrcLoc loc,
                                 TypeKind type) {
    Symbol *symbol = new_symbol(c, SYM_TYPE, name, loc, type);
    symbol->decl = stmt;
    symbol->exported = stmt->exported;
    symbol->visible_from = 0;
    declare(c, symbol);
    return symbol;
}

static bool member_repeats(Checker *c, const Stmt *stmt, size_t index, const char *what) {
    const Member *members = stmt->as.type.members;
    for (size_t i = 0; i < index; i++) {
        if (!str_eq(members[i].name, members[index].name)) continue;
        error(c, members[index].loc, "%s '%.*s' is already declared at line %d", what,
              NAME(members[index].name), members[i].loc.line);
        return true;
    }
    return false;
}

static Str default_module_name(Str path);

/* What diagnostics put in front of the name of a type declared in the file
   being checked: the file's name if it is an imported one. */
static Str type_qualifier(const Checker *c) {
    const Unit *unit = c->unit;
    if (unit->root == unit) return (Str){0};
    if (unit->module_name.len) return unit->module_name;
    return default_module_name((Str){unit->path, strlen(unit->path)});
}

/* Gives a struct its name and its type, with the fields still to come, so
   that any type in the file can point to it. */
static void declare_struct(Checker *c, Stmt *stmt) {
    TypeKind type = type_add(false, stmt->as.type.name, type_qualifier(c));
    type_info(type)->incomplete = true;
    type_info(type)->is_union = stmt->as.type.is_union;
    stmt->as.type.type = type;
    stmt->as.type.symbol = declare_type_name(c, stmt, stmt->as.type.name, stmt->loc, type);
    if (stmt->as.type.tag.len) {
        declare_type_name(c, stmt, stmt->as.type.tag, stmt->as.type.tag_loc, type);
    }
}

/* The width of a bitfield, "u8 ready : 1", checked: of an integer, a bool
   or an enum, as many bits as it can hold at most. 0 if it is wrong. */
static int bitfield_width(Checker *c, Member *member) {
    long long width;
    TypeKind type = member->type;
    TypeKind found = check_expr(c, member->bits, TYPE_INT);
    size_t most = type == TYPE_BOOL ? 1 : type_size(type) * 8;
    if (type == TYPE_INVALID || found == TYPE_INVALID) return 0;
    if (!(type_is_integer(type) || type == TYPE_BOOL || type_is_enum(type)) ||
        type == TYPE_I128 || type == TYPE_U128) {
        error(c, member->loc, "a bitfield is an integer, a 'bool' or an enum, not '%s'",
              type_name(type));
        return 0;
    }
    if (!type_is_integer(found) || !eval_const_int(member->bits, &width)) {
        error(c, member->bits->loc, "a bitfield's width must be an integer known at compile "
                                    "time");
        return 0;
    }
    if (width < 1 || (size_t)width > most) {
        error(c, member->bits->loc, "a bitfield of '%s' has from 1 to %zu bits, not %lld",
              type_name(type), most, width);
        return 0;
    }
    return (int)width;
}

static void check_struct_decl(Checker *c, Stmt *stmt) {
    size_t count = stmt->as.type.member_count;
    if (count == 0) error(c, stmt->loc, "a struct needs at least one field");
    Field *fields = arena_alloc(&c->unit->root->arena, (count ? count : 1) * sizeof *fields);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        Member *member = &stmt->as.type.members[i];
        member->type = declared_type(c, member->type, member->type_ref, member->loc,
                                     member->name);
        /* A struct held by value needs a size, which rules out the struct
           itself and those declared below it. */
        const TypeInfo *held = type_info(member->type);
        if (held && held->incomplete && member->type == stmt->as.type.type) {
            error(c, member->loc,
                  "a struct cannot hold itself; hold a pointer to it: '%s *%.*s'",
                  held->short_name, NAME(member->name));
            member->type = TYPE_INVALID;
        } else if (held && held->incomplete) {
            error(c, member->loc,
                  "struct '%s' is declared below, so this field can only hold a pointer "
                  "to it: '%s *%.*s'",
                  held->short_name, held->short_name, NAME(member->name));
            member->type = TYPE_INVALID;
        }
        if (member_repeats(c, stmt, i, "field")) continue;
        member->type = apply_suffixes(c, member->type, member->suffixes, member->suffix_count,
                                      SUFFIXES_FIELD, 0, member->name);
        if (stmt->as.type.is_union && type_owns(member->type)) {
            error(c, member->loc, "a union's fields share one place, so none can hold a "
                                  "growable array or a map, which would be lost or shared");
            member->type = TYPE_INVALID;
        }
        Field field = {member->name, member->loc, member->type, 0};
        if (member->bits) field.bits = bitfield_width(c, member);
        fields[kept++] = field;
    }
    TypeInfo *info = type_info(stmt->as.type.type);
    info->fields = fields;
    info->field_count = kept;
    for (size_t i = 0; i < kept; i++) {
        if (type_owns(fields[i].type)) info->owns = true;
    }
    type_layout(info);
    info->incomplete = false;
}

static void check_enum_decl(Checker *c, Stmt *stmt) {
    size_t count = stmt->as.type.member_count;
    if (count == 0) error(c, stmt->loc, "an enum needs at least one name");
    TypeKind type = type_add(true, stmt->as.type.name, type_qualifier(c));
    TypeInfo *info = type_info(type);
    info->values = arena_alloc(&c->unit->root->arena, (count ? count : 1) * sizeof *info->values);
    stmt->as.type.type = type;

    long long next = 0;
    for (size_t i = 0; i < count; i++) {
        Member *member = &stmt->as.type.members[i];
        if (member->value) {
            long long value;
            TypeKind found = check_expr(c, member->value, TYPE_INT);
            if (found == TYPE_INVALID) {
                /* already reported */
            } else if (!type_is_integer(found) || !eval_const_int(member->value, &value)) {
                error(c, member->value->loc,
                      "an enum value must be an integer known at compile time");
            } else {
                next = value;
            }
        }
        /* An enum is stored in 32 bits, as in C. */
        if (next < -2147483647LL - 1 || next > 2147483647LL) {
            error(c, member->loc, "the value of '%.*s' does not fit in 32 bits",
                  NAME(member->name));
            next = 0;
        }
        Symbol *symbol = new_symbol(c, SYM_ENUM_VALUE, member->name, member->loc, type);
        symbol->decl = stmt;
        symbol->exported = stmt->exported;
        symbol->visible_from = 0;
        symbol->value = next;
        member->symbol = symbol;
        declare(c, symbol);
        info->values[info->value_count++] = (EnumValue){member->name, next};
        next++;
    }
}

static void check_type_decl(Checker *c, Stmt *stmt) {
    switch (stmt->as.type.kind) {
    case TYPE_DECL_STRUCT:
        /* Its names are declared already; see declare_struct. */
        check_struct_decl(c, stmt);
        return;
    case TYPE_DECL_ENUM:
        check_enum_decl(c, stmt);
        break;
    case TYPE_DECL_ALIAS:
        if (stmt->as.type.type_ref) {
            stmt->as.type.type = resolve_type(c, stmt->as.type.type_ref);
        }
        break;
    }
    stmt->as.type.symbol = declare_type_name(c, stmt, stmt->as.type.name, stmt->loc,
                                             stmt->as.type.type);
    if (stmt->as.type.tag.len) {
        declare_type_name(c, stmt, stmt->as.type.tag, stmt->as.type.tag_loc,
                          stmt->as.type.type);
    }
}

/* ---- Modules ---- */

static void analyze(Unit *unit);

/* The name a module gets when neither its import nor its #module gives
   one: the last part of the path, the file's name without ".wind" or the
   folder's, after any directory and the "::" of the libraries. */
static Str default_module_name(Str path) {
    size_t end = path.len;
    while (end > 0 && path.data[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && path.data[start - 1] != '/' && path.data[start - 1] != ':') start--;
    Str name = {path.data + start, end - start};
    if (name.len > 5 && memcmp(name.data + name.len - 5, ".wind", 5) == 0) name.len -= 5;
    return name;
}

static bool is_usable_name(Str name) {
    if (name.len == 0) return false;
    for (size_t i = 0; i < name.len; i++) {
        char ch = name.data[i];
        bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
        bool digit = ch >= '0' && ch <= '9';
        if (!letter && !(digit && i > 0)) return false;
    }
    for (size_t i = 0; i < keyword_count(); i++) {
        if (str_eq_cstr(name, keyword_name(i))) return false;
    }
    return true;
}

static const char *arena_cstr(Arena *arena, const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = arena_alloc(arena, size);
    memcpy(copy, text, size);
    return copy;
}

/* Returns the unit for a file that an import brings in, analyzing the file
   if no import has reached it yet. Returns NULL, with the problem reported,
   if the file cannot be read or the import closes a cycle. */
static Unit *load_file(Checker *c, const Stmt *stmt, const char *path) {
    Unit *root = c->unit->root;
    Str written = stmt->as.import.path;
    char *real_path = path_canonical(path);
    Str src;
    if (!real_path) {
        error(c, stmt->loc, "cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    Unit *module = NULL;
    if (root->real_path && strcmp(root->real_path, real_path) == 0) module = root;
    for (size_t i = 0; !module && i < root->module_count; i++) {
        if (strcmp(root->modules[i]->real_path, real_path) == 0) module = root->modules[i];
    }
    if (module) {
        free(real_path);
        if (module->loading) {
            error(c, stmt->loc, "import cycle: '%.*s' leads back to itself",
                  NAME(written));
            return NULL;
        }
        return module;
    }

    if (!file_read(real_path, &src)) {
        error(c, stmt->loc, "cannot read '%s': %s", path, strerror(errno));
        free(real_path);
        return NULL;
    }
    module = arena_alloc(&root->arena, sizeof *module);
    memset(module, 0, sizeof *module);
    char *text = arena_alloc(&module->arena, src.len + 1);
    memcpy(text, src.data, src.len);
    free((char *)src.data);
    module->src = (Str){text, src.len};
    module->path = arena_cstr(&root->arena, path);
    module->real_path = arena_cstr(&root->arena, real_path);
    free(real_path);
    module->root = root;
    module->id = (int)root->module_count + 1;
    size_t index = root->module_count;
    ARENA_PUSH(&root->arena, root->modules, root->module_count, root->module_cap, module);

    analyze(module);

    /* Everything listed after the module was reached through it, so moving
       it to the end puts it behind all that it imports. */
    for (size_t i = index; i + 1 < root->module_count; i++) {
        root->modules[i] = root->modules[i + 1];
    }
    root->modules[root->module_count - 1] = module;
    return module;
}

/* Loads the files an import names: the file, or every ".wind" file in the
   folder, which sets *folder. Returns how many it loaded into *parts, from
   the root's arena, reporting each problem; 0 if there is nothing to use.
   *failed is set if any file could not be loaded. */
static size_t load_parts(Checker *c, const Stmt *stmt, const Unit ***parts, bool *failed,
                         bool *folder) {
    Unit *root = c->unit->root;
    Str written = stmt->as.import.path;
    *parts = NULL;
    *failed = false;
    *folder = false;
    bool folder_only;
    char *base = import_base(c->unit->path, written, &folder_only);
    if (!base) {
        error(c, stmt->loc, "'::' is the libraries directory, which was not found; set "
                            "WIND_LIBS to it");
        *failed = true;
        return 0;
    }
    bool has_extension = written.len >= 5 &&
                         memcmp(written.data + written.len - 5, ".wind", 5) == 0;
    char *file = xmalloc(strlen(base) + sizeof ".wind");
    sprintf(file, "%s%s", base, has_extension ? "" : ".wind");

    char **paths = NULL;
    bool too_large = false;
    size_t count = 0;
    if (!folder_only && !has_extension && !file_exists(file) && !is_directory(base)) {
        error(c, stmt->loc, "there is no file '%s' or folder '%s'", file, base);
        *failed = true;
    } else if (!folder_only && (has_extension || file_exists(file) || !is_directory(base))) {
        paths = xmalloc(sizeof *paths);
        paths[0] = file;
        file = NULL;
        count = 1;
    } else if (!is_directory(base)) {
        error(c, stmt->loc, "cannot open the folder '%s'", base);
        *failed = true;
    } else if (*folder = true, (count = wind_files_in(base, &paths, &too_large)) == 0) {
        if (too_large) {
            error(c, stmt->loc,
                  "the folder '%s' holds more than %d folders, or goes more than %d deep, "
                  "which is too large to be a module; import a folder further down",
                  base, WIND_FOLDER_LIMIT, WIND_FOLDER_DEPTH);
        } else {
            error(c, stmt->loc, "the folder '%s' holds no .wind files", base);
        }
        *failed = true;
    }
    free(file);
    free(base);

    const Unit **loaded = arena_alloc(&root->arena, (count ? count : 1) * sizeof *loaded);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const Unit *part = load_file(c, stmt, paths[i]);
        if (part) {
            loaded[kept++] = part;
        } else {
            *failed = true;
        }
        free(paths[i]);
    }
    free(paths);
    *parts = loaded;
    return kept;
}

/* The part of a path that tells it apart from the others of a folder: what
   follows the import's own path. */
static const char *part_name(const Unit *part) {
    const char *slash = strrchr(part->path, '/');
    return slash ? slash + 1 : part->path;
}

/* Reports a name that two parts of an import export, which a program could
   not tell apart. */
static void check_parts_apart(Checker *c, const Stmt *stmt, const Unit **parts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t k = 0; k < parts[i]->symbols.count; k++) {
            const Symbol *symbol = parts[i]->symbols.items[k];
            if (!symbol->exported || !symbol->global) continue;
            for (size_t j = i + 1; j < count; j++) {
                const Symbol *other = module_member(parts[j], symbol->name);
                if (!other || !other->exported) continue;
                error(c, stmt->loc, "'%.*s' is exported by both '%s' and '%s' of '%.*s'",
                      NAME(symbol->name), part_name(parts[i]), part_name(parts[j]),
                      NAME(stmt->as.import.path));
            }
        }
    }
}

/* What the "#module" of an import's parts calls it, or empty if none says.
   Parts that name it differently are reported. */
static Str parts_module_name(Checker *c, const Stmt *stmt, const Unit **parts, size_t count) {
    Str name = {0};
    const Unit *named_by = NULL;
    for (size_t i = 0; i < count; i++) {
        Str own = parts[i]->module_name;
        if (!own.len) continue;
        if (!named_by) {
            name = own;
            named_by = parts[i];
        } else if (!str_eq(own, name)) {
            error(c, stmt->loc, "'%s' names the module '%.*s' and '%s' names it '%.*s'",
                  part_name(named_by), NAME(name), part_name(parts[i]), NAME(own));
        }
    }
    return name;
}

static void check_import(Checker *c, Stmt *stmt) {
    Str path = stmt->as.import.path;
    if (path.len == 0 || memchr(path.data, '\0', path.len)) {
        error(c, stmt->loc, "an import needs the path of a file");
        return;
    }
    const Unit **parts;
    bool failed, folder;
    size_t count = load_parts(c, stmt, &parts, &failed, &folder);
    bool has_errors = false;
    for (size_t i = 0; i < count; i++) has_errors = has_errors || parts[i]->diags.error_count;
    if (has_errors) error(c, stmt->loc, "'%.*s' has errors", NAME(path));
    check_parts_apart(c, stmt, parts, count);
    bool broken = failed || has_errors || count == 0;

    bool has_alias = stmt->as.import.alias.len > 0;
    /* A module is a folder, which its files' #module names; a file imported
       on its own is named after the file. */
    Str own_name = folder ? parts_module_name(c, stmt, parts, count) : (Str){0};
    Str name = has_alias      ? stmt->as.import.alias
               : own_name.len ? own_name
                              : default_module_name(stmt->as.import.path);
    if (!is_usable_name(name)) {
        error(c, stmt->loc, "'%.*s' cannot be used as a name; give the import one with "
                            "'as name'",
              NAME(name));
        return;
    }
    Symbol *symbol = new_symbol(c, SYM_MODULE, name,
                                has_alias ? stmt->as.import.alias_loc : stmt->loc,
                                TYPE_INVALID);
    symbol->decl = stmt;
    symbol->parts = parts;
    symbol->part_count = count;
    symbol->visible_from = 0;
    /* An import that failed has its error; "never used" would add nothing. */
    symbol->used = broken;
    stmt->as.import.symbol = symbol;
    declare(c, symbol);
}

void check_program(Unit *unit) {
    Checker c;
    memset(&c, 0, sizeof c);
    c.unit = unit;
    c.arena = &unit->arena;
    c.diags = &unit->diags;
    c.locals = &unit->program.locals;

    Block *top = &unit->program.top;
    Scope global;
    push_scope(&c, &global, unit->src.len);
    c.global = &global;

    check_settings(&c);

    /* Imports, types and functions are declared before anything is checked,
       so they can be used from code above them, and functions from each
       other. */
    for (size_t i = 0; i < top->count; i++) {
        if (top->stmts[i]->kind == STMT_IMPORT) check_import(&c, top->stmts[i]);
    }
    for (size_t i = 0; i < top->count; i++) {
        Stmt *stmt = top->stmts[i];
        if (stmt->kind == STMT_TYPE && stmt->as.type.kind == TYPE_DECL_STRUCT) {
            declare_struct(&c, stmt);
        }
    }
    for (size_t i = 0; i < top->count; i++) {
        if (top->stmts[i]->kind == STMT_TYPE) check_type_decl(&c, top->stmts[i]);
    }
    for (size_t i = 0; i < top->count; i++) {
        Stmt *stmt = top->stmts[i];
        if (stmt->kind != STMT_FUNC) continue;
        if (stmt->as.func.ret_ref) {
            stmt->as.func.ret = resolve_type(&c, stmt->as.func.ret_ref);
        }
        if (type_owns(stmt->as.func.ret)) {
            report_copy(&c, stmt->loc, stmt->as.func.ret,
                        "fill one the caller points to instead");
        }
        if (stmt->as.func.variadic && !stmt->external) {
            error(&c, stmt->loc, "only an external function can take '...'; a Wind function "
                                 "takes an array of the values instead");
        }
        for (size_t j = 0; j < stmt->as.func.param_count; j++) {
            Param *param = &stmt->as.func.params[j];
            param->type = declared_type(&c, param->type, param->type_ref, param->loc,
                                        param->name);
            param->type = apply_suffixes(&c, param->type, param->suffixes, param->suffix_count,
                                         SUFFIXES_PARAM, 0, param->name);
            ContainerKind kind = type_container(param->type);
            if (param->is_const && (kind == CONTAINER_LIST || kind == CONTAINER_MAP)) {
                error(&c, param->const_loc, "a %s parameter is there to be changed, so it "
                                            "cannot be 'const'",
                      kind == CONTAINER_MAP ? "map" : "growable array");
            } else if (param->is_const && type_is_pointer(param->type)) {
                error(&c, param->const_loc, "'const' would not stop changes through pointer "
                                            "'%.*s'; Wind has no read-only pointers",
                      NAME(param->name));
            }

            if (!stmt->external) continue;
            carry_header(&c, stmt);
            bool carried = stmt->carried.len > 0;
            if (kind == CONTAINER_MAP) {
                error(&c, param->loc, "an external function cannot take a map");
            } else if (kind != CONTAINER_NONE && !carried) {
                error(&c, param->loc, "an external function cannot take an array; take a "
                                      "pointer to its elements");
            } else if (kind == CONTAINER_NONE) {
                check_crossing_to(&c, param->type, param->loc,
                                  "a parameter of an external function", carried);
            }
        }
        if (stmt->external) {
            carry_header(&c, stmt);
            check_crossing_to(&c, stmt->as.func.ret, stmt->loc, "an external function",
                              stmt->carried.len > 0);
        }
        Symbol *symbol = new_symbol(&c, SYM_FUNC, stmt->as.func.name, stmt->loc,
                                    stmt->as.func.ret);
        symbol->decl = stmt;
        symbol->exported = stmt->exported;
        symbol->visible_from = 0;
        stmt->as.func.symbol = symbol;
        declare(&c, symbol);
    }

    /* The function a platform calls over and over, in the program itself. */
    const char *repeat = types_platform()->repeat_function;
    const Symbol *repeated = repeat && unit->root == unit
                                 ? find_in(&global.symbols, (Str){repeat, strlen(repeat)})
                                 : NULL;
    if (repeated && repeated->kind == SYM_FUNC) {
        const Stmt *func = repeated->decl;
        if (func->as.func.param_count != 0 || func->as.func.ret != TYPE_VOID || func->external) {
            error(&c, func->loc, "'%s' is called over and over by %s, so it must be written "
                                 "'void %s()'",
                  repeat, types_platform()->name, repeat);
        }
    }

    const Symbol *main_symbol = find_in(&global.symbols, (Str){"main", 4});
    if (main_symbol && main_symbol->kind == SYM_FUNC) {
        check_main(&c, main_symbol->decl);
    } else if (unit->root != unit) {
        for (size_t i = 0; i < top->count; i++) {
            if (is_declaration(top->stmts[i])) continue;
            error(&c, top->stmts[i]->loc,
                  "an imported file can only have declarations at the top level; move "
                  "this statement into a function");
        }
    }

    collect_labels(&c, top);
    for (size_t i = 0; i < top->count; i++) check_stmt(&c, top->stmts[i]);
    check_goto_with_defer(&c);
    warn_after_exit(&c, top);
    warn_unused(&c);
}

/* Parses and checks a unit whose source and place among the program's files
   are set. */
static void analyze(Unit *unit) {
    unit->loading = true;
    unit->diags.arena = &unit->arena;
    unit->program = parse_program(unit->src, &unit->arena, &unit->diags);
    check_program(unit);
    diags_sort(&unit->diags);
    unit->loading = false;
}

void unit_analyze(Unit *unit, Str src, const char *path) {
    memset(unit, 0, sizeof *unit);
    unit->src = src;
    unit->root = unit;
    unit->types.arena = &unit->arena;
    types_use(&unit->types);
    if (path) {
        unit->path = arena_cstr(&unit->arena, path);
        char *real_path = path_canonical(path);
        if (real_path) unit->real_path = arena_cstr(&unit->arena, real_path);
        free(real_path);
    }
    analyze(unit);
}

void unit_release(Unit *unit) {
    for (size_t i = 0; i < unit->module_count; i++) arena_release(&unit->modules[i]->arena);
    arena_release(&unit->arena);
    memset(unit, 0, sizeof *unit);
}
