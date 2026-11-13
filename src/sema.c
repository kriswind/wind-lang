#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"

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
 * - An array is not a value: its name can only be indexed, measured with
 *   len() or sizeof, printed, or passed to an array parameter. A Symbol's type is
 *   then the type of the elements.
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
 * - Types are declared at the top level and are known throughout the file,
 *   but a type must be declared above another type that uses it.
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
    /* The innermost loop being checked, or NULL outside any loop. */
    const Stmt *loop;
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
static Symbol *resolve(Checker *c, Str name, SrcLoc loc) {
    Symbol *symbol = lookup(c, name);
    if (!symbol) {
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
    if (!owner->module) return NULL;
    Symbol *symbol = module_member(owner->module, name);
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
static const Symbol *array_operand(Checker *c, Expr *expr);

/* If expr is the name of a type, possibly one that a module exports,
   returns the type's symbol; NULL, with nothing reported, otherwise. */
static Symbol *type_named_by(const Checker *c, const Expr *expr) {
    if (expr->kind != EXPR_IDENT) return NULL;
    Symbol *symbol;
    if (expr->as.ident.module.len) {
        const Symbol *owner = lookup(c, expr->as.ident.module);
        if (!owner || owner->kind != SYM_MODULE || !owner->module) return NULL;
        symbol = module_member(owner->module, expr->as.ident.name);
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

/* Finds the type that a type written with a name or typeof stands for.
   Returns TYPE_INVALID, with the problem reported, if there is none. */
static TypeKind resolve_type(Checker *c, const TypeRef *ref) {
    if (ref->of) {
        Symbol *named = type_named_by(c, ref->of);
        if (named) {
            use_type_name(c, ref->of, named);
            return named->type;
        }
        if (array_operand(c, ref->of)) {
            error(c, ref->of->loc, "typeof needs a value, and an array is not one; use "
                                   "typeof on one of its elements");
            return TYPE_INVALID;
        }
        return check_expr(c, ref->of, TYPE_INVALID);
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
        if (!info || info->is_enum) {
            error(c, fields[i].loc, "type '%s' has no fields", type_name(type));
            return TYPE_INVALID;
        }
        fields[i].index = type_field_index(info, fields[i].name);
        if (fields[i].index < 0) {
            error(c, fields[i].loc, "struct '%s' has no field '%.*s'", info->name,
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
    const TypeInfo *info = type_info(base);
    if (!info || info->is_enum) {
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

/* Checks a "{ ... }" value against the struct type its context asks for. */
static TypeKind check_init(Checker *c, Expr *expr, TypeKind hint) {
    const TypeInfo *info = type_info(hint);
    if (!info || info->is_enum) {
        if (hint == TYPE_INVALID) {
            error(c, expr->loc, "this '{ ... }' has nothing to tell it which struct it "
                                "is; write the type first, as in '(Type){ ... }'");
        } else {
            error(c, expr->loc, "type '%s' cannot be written as '{ ... }'", type_name(hint));
        }
        for (size_t i = 0; i < expr->as.init.count; i++) {
            check_expr(c, expr->as.init.items[i].value, TYPE_INVALID);
        }
        return TYPE_INVALID;
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
            check_expr(c, item->value, TYPE_INVALID);
            continue;
        }
        for (size_t j = 0; j < i; j++) {
            if (expr->as.init.items[j].index == item->index) {
                error(c, item->value->loc, "field '%.*s' is given a value twice",
                      NAME(info->fields[item->index].name));
                break;
            }
        }
        TypeKind expected = info->fields[item->index].type;
        TypeKind found = check_expr(c, item->value, expected);
        if (expected != TYPE_INVALID && found != TYPE_INVALID && found != expected) {
            error(c, item->value->loc, "field '%.*s' is '%s', found '%s'",
                  NAME(info->fields[item->index].name), type_name(expected),
                  type_name(found));
        }
    }
    return hint;
}

/* If expr is the name of an array, resolves it and returns its symbol, with
   expr->type set to the element type. Returns NULL, having done nothing, for
   any other expression. */
static const Symbol *array_operand(Checker *c, Expr *expr) {
    if (expr->kind != EXPR_IDENT) return NULL;
    Str module = expr->as.ident.module;
    Symbol *symbol;
    if (module.len) {
        const Symbol *owner = lookup(c, module);
        if (!owner || owner->kind != SYM_MODULE || !owner->module) return NULL;
        symbol = module_member(owner->module, expr->as.ident.name);
        if (symbol && !symbol->exported) return NULL;
    } else {
        symbol = lookup(c, expr->as.ident.name);
    }
    if (!symbol || (!symbol->is_array && !symbol->is_slice)) return NULL;
    if (module.len) {
        resolve_member(c, module, expr->as.ident.module_loc, expr->as.ident.name,
                       expr->as.ident.name_loc);
    } else {
        add_ref(c, expr->as.ident.name_loc, symbol);
    }
    symbol->used = true;
    expr->as.ident.symbol = symbol;
    expr->type = symbol->type;
    return symbol;
}

/* Checks an expression used as an index. A negative index counts back from
   the end, so an array of length n accepts -n to n - 1. For a fixed array a
   literal index is range-checked here; everything else is checked when the
   program runs. */
static void check_index_value(Checker *c, Expr *index, bool fixed, size_t length) {
    TypeKind type = check_expr(c, index, TYPE_INT);
    if (type == TYPE_INVALID) return;
    if (!type_is_integer(type) || type == TYPE_I128 || type == TYPE_U128) {
        error(c, index->loc, "an index must be an integer of at most 64 bits, found '%s'",
              type_name(type));
    } else if (fixed && index->kind == EXPR_UNARY && index->as.unary.op == OP_NEG &&
               index->as.unary.operand->kind == EXPR_INT_LIT &&
               index->as.unary.operand->as.int_lit > length) {
        error(c, index->loc, "index -%llu is out of range for an array of length %zu",
              (unsigned long long)index->as.unary.operand->as.int_lit, length);
    } else if (fixed && index->kind == EXPR_INT_LIT && index->as.int_lit >= length) {
        error(c, index->loc, "index %llu is out of range for an array of length %zu",
              (unsigned long long)index->as.int_lit, length);
    }
}

static TypeKind check_index(Checker *c, Expr *expr) {
    Expr *base = expr->as.index.base;
    const Symbol *array = array_operand(c, base);
    TypeKind result = TYPE_INVALID;
    if (array) {
        result = array->type;
    } else {
        TypeKind type = check_expr(c, base, TYPE_INVALID);
        if (type == TYPE_STRING) {
            result = TYPE_CHAR;
        } else if (type != TYPE_INVALID) {
            error(c, base->loc, "cannot index a value of type '%s'; only arrays and "
                                "strings have elements",
                  type_name(type));
        }
    }
    check_index_value(c, expr->as.index.index, array && array->is_array,
                      array ? array->length : 0);
    return result;
}

/* The built-in len(x): the number of elements of an array or bytes of a
   string. Used unless the program declares its own "len". */
static bool is_builtin_len(const Checker *c, const Expr *expr) {
    return !expr->as.call.module.len && str_eq_cstr(expr->as.call.name, "len") &&
           !lookup(c, expr->as.call.name);
}

static TypeKind check_len(Checker *c, Expr *expr) {
    if (expr->as.call.count != 1) {
        error(c, expr->loc, "'len' takes 1 argument, but %zu were given",
              expr->as.call.count);
        for (size_t i = 0; i < expr->as.call.count; i++) {
            if (!array_operand(c, expr->as.call.args[i])) {
                check_expr(c, expr->as.call.args[i], TYPE_INVALID);
            }
        }
        return TYPE_INT;
    }
    Expr *operand = expr->as.call.args[0];
    if (!array_operand(c, operand)) {
        TypeKind type = check_expr(c, operand, TYPE_INVALID);
        if (type != TYPE_STRING && type != TYPE_INVALID) {
            error(c, operand->loc, "'len' needs an array or a string, found '%s'",
                  type_name(type));
        }
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

static TypeKind check_unary(Checker *c, Expr *expr, TypeKind hint) {
    Expr *operand = expr->as.unary.operand;
    if (expr->as.unary.op == OP_NOT) {
        TypeKind type = check_expr(c, operand, TYPE_INVALID);
        if (type != TYPE_BOOL && type != TYPE_INVALID) {
            error(c, operand->loc, "operator '!' needs a 'bool' operand, found '%s'",
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

static TypeKind check_binary(Checker *c, Expr *expr, TypeKind hint) {
    Op op = expr->as.binary.op;
    Expr *left = expr->as.binary.left;
    Expr *right = expr->as.binary.right;

    if (op == OP_AND || op == OP_OR) {
        Expr *sides[2] = {left, right};
        for (int i = 0; i < 2; i++) {
            TypeKind type = check_expr(c, sides[i], TYPE_INVALID);
            if (type != TYPE_BOOL && type != TYPE_INVALID) {
                error(c, sides[i]->loc, "operator '%s' needs 'bool' operands, found '%s'",
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
    if (left->type != right->type) {
        error(c, expr->loc, "mismatched types '%s' and '%s' for operator '%s'",
              type_name(left->type), type_name(right->type), op_text(op));
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
        defined = type_is_numeric(type) || type == TYPE_CHAR || type_is_enum(type);
    }
    if (!defined) {
        error(c, expr->loc, "operator '%s' is not defined for '%s'", op_text(op),
              type_name(type));
        return arithmetic ? TYPE_INVALID : TYPE_BOOL;
    }
    return result;
}

static TypeKind check_ternary(Checker *c, Expr *expr, TypeKind hint) {
    Expr *cond = expr->as.ternary.cond;
    Expr *then = expr->as.ternary.then;
    Expr *otherwise = expr->as.ternary.otherwise;

    TypeKind cond_type = check_expr(c, cond, TYPE_INVALID);
    if (cond_type != TYPE_BOOL && cond_type != TYPE_INVALID) {
        error(c, cond->loc, "the condition of '?:' must be 'bool', found '%s'",
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

static TypeKind check_call(Checker *c, Expr *expr) {
    Str name = expr->as.call.name;
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

    size_t count = expr->as.call.count;
    if (func && count != func->as.func.param_count) {
        error(c, expr->loc, "'%.*s' takes %zu argument%s, but %zu %s given", NAME(name),
              func->as.func.param_count, func->as.func.param_count == 1 ? "" : "s",
              count, count == 1 ? "was" : "were");
    }
    for (size_t i = 0; i < count; i++) {
        Expr *arg = expr->as.call.args[i];
        bool has_param = func && i < func->as.func.param_count;
        TypeKind expected = has_param ? func->as.func.params[i].type : TYPE_INVALID;
        if (has_param && func->as.func.params[i].is_slice) {
            const Symbol *array = array_operand(c, arg);
            if (!array) check_expr(c, arg, TYPE_INVALID);
            if (!array || array->type != expected) {
                error(c, arg->loc, "argument %zu of '%.*s' must be an array of '%s'", i + 1,
                      NAME(name), type_name(expected));
            } else if (array->kind == SYM_CONST) {
                error(c, arg->loc,
                      "constant array '%.*s' cannot be passed to a function, which "
                      "could change it",
                      NAME(array->name));
            }
            continue;
        }
        TypeKind type = check_expr(c, arg, expected);
        if (expected != TYPE_INVALID && type != TYPE_INVALID && type != expected) {
            error(c, arg->loc, "argument %zu of '%.*s' must be '%s', found '%s'", i + 1,
                  NAME(name), type_name(expected), type_name(type));
        }
    }
    return func ? func->as.func.ret : TYPE_INVALID;
}

static TypeKind check_cast(Checker *c, Expr *expr) {
    if (expr->as.cast.to_ref) {
        expr->as.cast.to = resolve_type(c, expr->as.cast.to_ref);
        expr->as.cast.to_ref = NULL;
    }
    TypeKind to = expr->as.cast.to;
    Expr *operand = expr->as.cast.operand;
    /* A struct type in front of "{ ... }" is what gives that value its
       type, as in C's "(Point){ 1, 2 }". */
    bool hinted = type_is_numeric(to) || type_is_struct(to);
    TypeKind from = check_expr(c, operand, hinted ? to : TYPE_INVALID);
    if (from == TYPE_INVALID || to == TYPE_INVALID) return to;

    bool allowed = from == to ||
                   (type_is_numeric(from) && type_is_numeric(to)) ||
                   (from == TYPE_CHAR && type_is_integer(to)) ||
                   (type_is_integer(from) && to == TYPE_CHAR) ||
                   (type_is_enum(from) && type_is_integer(to)) ||
                   (type_is_integer(from) && type_is_enum(to));
    if (!allowed || to == TYPE_VOID) {
        error(c, expr->loc, "cannot convert '%s' to '%s'", type_name(from),
              type_name(to));
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
        return TYPE_STRING;
    case EXPR_BOOL_LIT:
        return TYPE_BOOL;
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
                expr->as.field.index = -1;
                return check_field(c, expr);
            }
        }
        Symbol *symbol = expr->as.ident.module.len
                             ? resolve_member(c, expr->as.ident.module,
                                              expr->as.ident.module_loc,
                                              expr->as.ident.name, expr->as.ident.name_loc)
                             : resolve(c, expr->as.ident.name, expr->loc);
        if (!symbol) return TYPE_INVALID;
        if (symbol->kind == SYM_FUNC) {
            error(c, expr->loc, "'%.*s' is a function; call it with '()'",
                  NAME(expr->as.ident.name));
            return TYPE_INVALID;
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
        if (symbol->is_array || symbol->is_slice) {
            error(c, expr->loc,
                  "array '%.*s' cannot be used as a value; index it, print it, or pass "
                  "it to an array parameter",
                  NAME(expr->as.ident.name));
            return TYPE_INVALID;
        }
        expr->as.ident.symbol = symbol;
        return symbol->type;
    }
    case EXPR_INDEX:
        return check_index(c, expr);
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
        return is_builtin_len(c, expr) ? check_len(c, expr) : check_call(c, expr);
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
        size_t count = 1;
        if (expr->as.size_of.type_ref) of = resolve_type(c, expr->as.size_of.type_ref);
        Symbol *named = expr->as.size_of.operand
                            ? type_named_by(c, expr->as.size_of.operand)
                            : NULL;
        if (named) {
            use_type_name(c, expr->as.size_of.operand, named);
            of = named->type;
        } else if (expr->as.size_of.operand) {
            const Symbol *array = array_operand(c, expr->as.size_of.operand);
            if (array && array->is_array) {
                of = array->type;
                count = array->length;
            } else if (array) {
                of = TYPE_STRING; /* a slice is also a pointer and a length */
            } else {
                of = check_expr(c, expr->as.size_of.operand, TYPE_INVALID);
            }
        }
        if (of == TYPE_VOID) error(c, expr->loc, "cannot take the size of 'void'");
        expr->kind = EXPR_INT_LIT;
        expr->as.int_lit = type_size(of) * count;
        return check_int_literal(c, expr, hint, false);
    }
    }
    return TYPE_INVALID;
}

/* hint is the type the context would like, or TYPE_INVALID for none. It only
   steers untyped literals; the caller still compares the result. */
static TypeKind check_expr(Checker *c, Expr *expr, TypeKind hint) {
    if (is_untyped(expr) && !type_is_numeric(hint)) {
        hint = has_float_literal(expr) ? TYPE_FLOAT : TYPE_INT;
    }
    expr->type = check_expr_kind(c, expr, hint);
    return expr->type;
}

/* If the operand of len() has a length known at compile time, sets *length
   and returns true: a fixed array, a string literal, or a string constant. */
static bool const_len(const Expr *operand, size_t *length) {
    while (operand->kind == EXPR_IDENT) {
        const Symbol *symbol = operand->as.ident.symbol;
        if (!symbol) return false;
        if (symbol->is_array) {
            *length = symbol->length;
            return true;
        }
        if (symbol->kind != SYM_CONST || symbol->is_slice) return false;
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
        return true;
    case EXPR_IDENT:
        return !expr->as.ident.symbol || expr->as.ident.symbol->kind == SYM_CONST ||
               expr->as.ident.symbol->kind == SYM_ENUM_VALUE;
    case EXPR_UNARY:
        return is_constant(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_constant(expr->as.binary.left) && is_constant(expr->as.binary.right);
    case EXPR_CAST:
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
        if (!symbol || symbol->kind != SYM_CONST || symbol->is_array) return false;
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
        if (!symbol || symbol->kind != SYM_CONST || symbol->is_array) return false;
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

static void check_array_decl(Checker *c, Stmt *stmt) {
    stmt->as.decl.type = declared_type(c, stmt->as.decl.type, stmt->as.decl.type_ref,
                                       stmt->loc, stmt->as.decl.name);
    TypeKind type = stmt->as.decl.type;
    bool is_const = stmt->kind == STMT_CONST;
    size_t count = stmt->as.decl.elem_count;
    size_t length = count;
    bool length_known = true;

    Expr *length_expr = stmt->as.decl.length;
    if (length_expr) {
        long long value;
        TypeKind found = check_expr(c, length_expr, TYPE_INT);
        length_known = false;
        if (found == TYPE_INVALID) {
            /* already reported */
        } else if (!type_is_integer(found) || !eval_const_int(length_expr, &value)) {
            error(c, length_expr->loc,
                  "an array's length must be an integer known at compile time");
        } else if (value < 1 || value > MAX_ARRAY_LENGTH) {
            error(c, length_expr->loc, "an array's length must be between 1 and %lld",
                  MAX_ARRAY_LENGTH);
        } else {
            length = (size_t)value;
            length_known = true;
        }
    } else if (count == 0) {
        error(c, stmt->loc, "an array needs at least one element");
        length_known = false;
    }

    for (size_t i = 0; i < count; i++) {
        Expr *elem = stmt->as.decl.elems[i];
        TypeKind found = check_expr(c, elem, type);
        if (is_const) elem = stmt->as.decl.elems[i] = settle_constant(elem);
        if (type == TYPE_INVALID) continue;
        if (found != TYPE_INVALID && found != type) {
            error(c, elem->loc, "an element of this array must be '%s', found '%s'",
                  type_name(type), type_name(found));
        } else if (is_const && !is_constant(elem)) {
            error(c, elem->loc, "a constant's value must be known at compile time; use "
                                "a variable for this");
        }
    }
    if (length_known && count > length) {
        error(c, stmt->as.decl.elems[length]->loc,
              "too many elements: the array has length %zu", length);
    }

    Symbol *symbol = new_symbol(c, is_const ? SYM_CONST : SYM_VAR, stmt->as.decl.name,
                                stmt->loc, type);
    symbol->decl = stmt;
    symbol->exported = stmt->exported;
    symbol->is_array = true;
    symbol->length = length_known && length ? length : 1;
    stmt->as.decl.symbol = symbol;
    declare(c, symbol);
    if (!is_const) {
        list_push(c, symbol->global ? &c->unit->program.globals : c->locals, symbol);
    }
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

    /* The initializer is checked before the name is declared, so a
       declaration cannot refer to itself. */
    if (init) {
        TypeKind found = check_expr(c, init, type);
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

    Symbol *symbol = new_symbol(c, is_const ? SYM_CONST : SYM_VAR, stmt->as.decl.name,
                                stmt->loc, type);
    symbol->decl = stmt;
    symbol->exported = stmt->exported;
    stmt->as.decl.symbol = symbol;
    declare(c, symbol);
    if (!is_const) {
        list_push(c, symbol->global ? &c->unit->program.globals : c->locals, symbol);
    }
}

static void check_assign(Checker *c, Stmt *stmt) {
    Str name = stmt->as.assign.name;
    Symbol *symbol = resolve(c, name, stmt->loc);
    Expr *index = stmt->as.assign.index;
    TypeKind expected = TYPE_INVALID;
    bool is_array = symbol && (symbol->is_array || symbol->is_slice);
    if (symbol && index) {
        /* The target is one element. An array parameter's elements can be
           assigned even though the parameter itself cannot. */
        if (!is_array) {
            error(c, stmt->loc,
                  symbol->type == TYPE_STRING && symbol->kind != SYM_FUNC
                      ? "'%.*s' is a string, and a string's characters cannot be changed"
                      : "'%.*s' is not an array",
                  NAME(name));
        } else if (symbol->kind == SYM_CONST) {
            error(c, stmt->loc, "cannot assign to an element of constant array '%.*s'",
                  NAME(name));
        } else {
            stmt->as.assign.symbol = symbol;
            expected = symbol->type;
        }
        check_index_value(c, index, symbol->is_array, symbol->length);
    } else if (is_array) {
        error(c, stmt->loc, "cannot assign to array '%.*s' as a whole; assign its elements",
              NAME(name));
    } else if (symbol && symbol->kind == SYM_MODULE && stmt->as.assign.field_count) {
        symbol->used = true;
        error(c, stmt->loc, "what a module exports can be called or read, but not changed");
    } else if (symbol && symbol->kind != SYM_VAR) {
        error(c, stmt->loc, "cannot assign to %s '%.*s'", symbol_what(symbol), NAME(name));
    } else if (symbol) {
        stmt->as.assign.symbol = symbol;
        expected = symbol->type;
    } else if (index) {
        check_expr(c, index, TYPE_INT);
    }
    bool has_fields = stmt->as.assign.field_count > 0;
    if (has_fields) {
        expected = check_place_fields(c, expected, stmt->as.assign.fields,
                                      stmt->as.assign.field_count);
    }
    stmt->as.assign.type = expected;
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
    TypeKind found = check_expr(c, stmt->as.assign.value, expected);
    if (expected == TYPE_INVALID) return;

    bool needs_integer = op == OP_MOD || is_bitwise(op);
    if (compound && !(needs_integer ? type_is_integer(expected) : type_is_numeric(expected))) {
        error(c, stmt->loc, "operator '%s=' is not defined for '%s'", op_text(op),
              type_name(expected));
    } else if (found != TYPE_INVALID && found != expected) {
        error(c, stmt->as.assign.value->loc, "cannot assign '%s' to '%s' %s '%.*s'",
              type_name(found), type_name(expected),
              has_fields ? "field of" : index ? "element of" : "variable", NAME(name));
    }
}

static void check_return(Checker *c, Stmt *stmt) {
    Expr *value = stmt->as.ret.value;
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
        if (stmt->kind == STMT_IF) {
            collect_labels(c, &stmt->as.if_.then_block);
            if (stmt->as.if_.has_else) collect_labels(c, &stmt->as.if_.else_block);
        } else if (stmt->kind == STMT_WHILE || stmt->kind == STMT_DO_WHILE ||
                   stmt->kind == STMT_FOR) {
            collect_labels(c, &stmt->as.loop.body);
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

/* True if control cannot run off the end of the block. Conservative: it only
   recognizes a return or goto, an if/else whose branches both qualify, and a
   loop that never ends. Whatever follows such a statement cannot be reached,
   unless a label lets a goto back in. */
static bool block_terminates(const Block *block) {
    bool exits = false;
    for (size_t i = 0; i < block->count; i++) {
        const Stmt *stmt = block->stmts[i];
        if (stmt_has_label(stmt)) exits = false;
        switch (stmt->kind) {
        case STMT_RETURN:
        case STMT_GOTO:
            exits = true;
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
        default:
            break;
        }
    }
    return exits;
}

static void check_func(Checker *c, Stmt *stmt) {
    Stmt *outer_func = c->func;
    SymbolList *outer_locals = c->locals;
    SymbolList outer_labels = c->labels;
    Scope *outer_scope = c->scope;
    const Stmt *outer_loop = c->loop;

    /* A function body sees the top level only. */
    c->scope = c->global;
    c->func = stmt;
    c->loop = NULL;
    c->locals = &stmt->as.func.locals;
    memset(&c->labels, 0, sizeof c->labels);

    Block *body = &stmt->as.func.body;
    Scope scope;
    push_scope(c, &scope, body->close.offset);
    for (size_t i = 0; i < stmt->as.func.param_count; i++) {
        Param *param = &stmt->as.func.params[i];
        Symbol *symbol = new_symbol(c, SYM_PARAM, param->name, param->loc, param->type);
        symbol->is_slice = param->is_slice;
        symbol->visible_from = body->open.offset;
        param->symbol = symbol;
        declare(c, symbol);
    }
    collect_labels(c, body);
    for (size_t i = 0; i < body->count; i++) check_stmt(c, body->stmts[i]);
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
    c->scope = outer_scope;
}

/* ---- Unreachable code ---- */

static bool block_has_label(const Block *block);

static bool stmt_has_label(const Stmt *stmt) {
    switch (stmt->kind) {
    case STMT_LABEL:
        return true;
    case STMT_IF:
        return block_has_label(&stmt->as.if_.then_block) ||
               (stmt->as.if_.has_else && block_has_label(&stmt->as.if_.else_block));
    case STMT_WHILE:
    case STMT_DO_WHILE:
    case STMT_FOR:
        return block_has_label(&stmt->as.loop.body);
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
    if (type != TYPE_BOOL && type != TYPE_INVALID) {
        error(c, cond->loc, "condition must be 'bool', found '%s'", type_name(type));
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

    c->loop = stmt;
    check_block(c, body);
    c->loop = outer_loop;
    /* A do-while runs its body once whatever the condition says. */
    if (stmt->kind != STMT_DO_WHILE && is_literal(stmt->as.loop.cond, false)) {
        warn_dead_block(c, body, "the loop's condition is always false");
    }

    /* A do-while's condition is outside the body's scope, as in C. */
    if (stmt->kind == STMT_DO_WHILE) check_condition(c, stmt->as.loop.cond);
    pop_scope(c);
}

/* Settles what a read into a variable, or one of its elements, stores: the
   place its ".field" steps lead to, which must be of a type that input can
   be converted to. */
static void accept_read_target(Checker *c, ReadTarget *target, const Symbol *symbol) {
    TypeKind type = check_place_fields(c, symbol->type, target->fields, target->field_count);
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
    case STMT_BREAK:
    case STMT_CONTINUE:
        stmt->as.target = c->loop;
        if (!c->loop) {
            error(c, stmt->loc, "'%s' outside of a loop",
                  stmt->kind == STMT_BREAK ? "break" : "continue");
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
            /* An array's name can be printed whole. */
            if (array_operand(c, arg)) continue;
            if (check_expr(c, arg, TYPE_INVALID) == TYPE_VOID) {
                error(c, arg->loc, "cannot print a 'void' value");
            }
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
            bool is_array = symbol->is_array || symbol->is_slice;
            if (target->index) {
                check_index_value(c, target->index, symbol->is_array, symbol->length);
                if (!is_array) {
                    error(c, target->loc, "'%.*s' is not an array", NAME(target->name));
                } else if (symbol->kind == SYM_CONST) {
                    error(c, target->loc,
                          "cannot read into an element of constant array '%.*s'",
                          NAME(target->name));
                } else {
                    accept_read_target(c, target, symbol);
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
            accept_read_target(c, target, symbol);
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
    case STMT_IMPORT:
    case STMT_TYPE:
        break; /* handled before anything else is checked */
    }
}

static void warn_unused(Checker *c) {
    for (size_t i = 0; i < c->unit->symbols.count; i++) {
        const Symbol *symbol = c->unit->symbols.items[i];
        if (symbol->kind == SYM_PARAM || symbol->kind == SYM_TYPE ||
            symbol->kind == SYM_ENUM_VALUE) {
            continue;
        }
        /* main is called by the runtime, and what a file exports is there
           for the files that import it. */
        if (symbol->decl && symbol->decl == c->unit->program.main) continue;
        if (symbol->exported) continue;
        if (!symbol->used) {
            diag_add(c->diags, SEV_WARNING, symbol->loc, "%s '%.*s' is never used",
                     symbol_what(symbol), NAME(symbol->name));
        }
    }
}

static bool is_declaration(const Stmt *stmt) {
    return stmt->kind == STMT_CONST || stmt->kind == STMT_VAR ||
           stmt->kind == STMT_FUNC || stmt->kind == STMT_IMPORT ||
           stmt->kind == STMT_TYPE;
}

/* A file that defines main is laid out like a C program: declarations at the
   top level, and main as the entry point. */
static void check_main(Checker *c, const Stmt *main) {
    c->unit->program.main = main;
    const Param *params = main->as.func.params;
    bool takes_args = main->as.func.param_count == 2 && params[0].type == TYPE_INT &&
                      !params[0].is_slice && params[1].type == TYPE_STRING &&
                      params[1].is_slice;
    if (main->as.func.param_count != 0 && !takes_args) {
        error(c, main->loc,
              "'main' takes no parameters, or the program's arguments as "
              "'(int argc, string argv[])'");
    }
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
    return default_module_name((Str){unit->path, strlen(unit->path)});
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
        if (member_repeats(c, stmt, i, "field")) continue;
        fields[kept++] = (Field){member->name, member->loc, member->type};
    }
    /* The type is added after its fields are checked, so that a field
       cannot be of the struct's own type, which would have no size. */
    TypeKind type = type_add(false, stmt->as.type.name, type_qualifier(c));
    TypeInfo *info = type_info(type);
    info->fields = fields;
    info->field_count = kept;
    type_layout(info);
    stmt->as.type.type = type;
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
        check_struct_decl(c, stmt);
        break;
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

/* The name a module gets when its import does not give one: the file's name
   without its directory and ".wind". */
static Str default_module_name(Str path) {
    size_t start = path.len;
    while (start > 0 && path.data[start - 1] != '/') start--;
    Str name = {path.data + start, path.len - start};
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

/* Returns the unit for the file an import names, analyzing the file if no
   import has reached it yet. Returns NULL, with the problem reported, if
   the file cannot be read or the import closes a cycle. */
static const Unit *load_module(Checker *c, const Stmt *stmt) {
    Unit *root = c->unit->root;
    Str written = stmt->as.import.path;
    char *path = path_beside(c->unit->path, written);
    char *real_path = path_canonical(path);
    Str src;
    if (!real_path) {
        error(c, stmt->loc, "cannot open '%s': %s", path, strerror(errno));
        free(path);
        return NULL;
    }

    Unit *module = NULL;
    if (root->real_path && strcmp(root->real_path, real_path) == 0) module = root;
    for (size_t i = 0; !module && i < root->module_count; i++) {
        if (strcmp(root->modules[i]->real_path, real_path) == 0) module = root->modules[i];
    }
    if (module) {
        free(path);
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
        free(path);
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
    free(path);
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

static void check_import(Checker *c, Stmt *stmt) {
    Str path = stmt->as.import.path;
    if (path.len == 0 || memchr(path.data, '\0', path.len)) {
        error(c, stmt->loc, "an import needs the path of a file");
        return;
    }
    const Unit *module = load_module(c, stmt);
    bool broken = !module || module->diags.error_count > 0;
    if (module && broken) error(c, stmt->loc, "'%.*s' has errors", NAME(path));

    bool has_alias = stmt->as.import.alias.len > 0;
    Str name = has_alias ? stmt->as.import.alias : default_module_name(stmt->as.import.path);
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
    symbol->module = module;
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

    /* Imports, types and functions are declared before anything is checked,
       so they can be used from code above them, and functions from each
       other. */
    for (size_t i = 0; i < top->count; i++) {
        if (top->stmts[i]->kind == STMT_IMPORT) check_import(&c, top->stmts[i]);
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
        for (size_t j = 0; j < stmt->as.func.param_count; j++) {
            Param *param = &stmt->as.func.params[j];
            param->type = declared_type(&c, param->type, param->type_ref, param->loc,
                                        param->name);
        }
        Symbol *symbol = new_symbol(&c, SYM_FUNC, stmt->as.func.name, stmt->loc,
                                    stmt->as.func.ret);
        symbol->decl = stmt;
        symbol->exported = stmt->exported;
        symbol->visible_from = 0;
        stmt->as.func.symbol = symbol;
        declare(&c, symbol);
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
