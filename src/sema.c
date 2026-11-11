#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ast.h"

/*
 * Typing rules:
 *
 * - There are no implicit conversions: both sides of an operator, an
 *   initializer and its variable, an argument and its parameter must have the
 *   same type. T(x) converts explicitly.
 * - Number literals are untyped until used: they take the type the context
 *   asks for (the "hint"), and default to int or float without one.
 *
 * Scoping rules:
 *
 * - Names must be declared before use, except functions and labels.
 * - A function sees what is declared directly at the top level: every
 *   function, and the constants and variables declared above it.
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

/* ---- Expressions ---- */

static TypeKind check_expr(Checker *c, Expr *expr, TypeKind hint);

static bool is_arithmetic(Op op) {
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV ||
           op == OP_MOD;
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
        return expr->as.unary.op == OP_NEG && is_untyped(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_arithmetic(expr->as.binary.op) &&
               is_untyped(expr->as.binary.left) && is_untyped(expr->as.binary.right);
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

    bool arithmetic = is_arithmetic(op);
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
        defined = type != TYPE_VOID;
    } else if (op == OP_MOD) {
        defined = type_is_integer(type);
    } else if (arithmetic) {
        defined = type_is_numeric(type);
    } else {
        defined = type_is_numeric(type) || type == TYPE_CHAR;
    }
    if (!defined) {
        error(c, expr->loc, "operator '%s' is not defined for '%s'", op_text(op),
              type_name(type));
        return arithmetic ? TYPE_INVALID : TYPE_BOOL;
    }
    return result;
}

static TypeKind check_call(Checker *c, Expr *expr) {
    Str name = expr->as.call.name;
    Symbol *symbol = resolve(c, name, expr->as.call.name_loc);
    const Stmt *func = NULL;
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
        TypeKind type = check_expr(c, arg, expected);
        if (expected != TYPE_INVALID && type != TYPE_INVALID && type != expected) {
            error(c, arg->loc, "argument %zu of '%.*s' must be '%s', found '%s'", i + 1,
                  NAME(name), type_name(expected), type_name(type));
        }
    }
    return func ? func->as.func.ret : TYPE_INVALID;
}

static TypeKind check_cast(Checker *c, Expr *expr) {
    TypeKind to = expr->as.cast.to;
    Expr *operand = expr->as.cast.operand;
    TypeKind from = check_expr(c, operand, type_is_numeric(to) ? to : TYPE_INVALID);
    if (from == TYPE_INVALID) return to;

    bool allowed = from == to ||
                   (type_is_numeric(from) && type_is_numeric(to)) ||
                   (from == TYPE_CHAR && type_is_integer(to)) ||
                   (type_is_integer(from) && to == TYPE_CHAR);
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
        Symbol *symbol = resolve(c, expr->as.ident.name, expr->loc);
        if (!symbol) return TYPE_INVALID;
        if (symbol->kind == SYM_FUNC) {
            error(c, expr->loc, "'%.*s' is a function; call it with '()'",
                  NAME(expr->as.ident.name));
            return TYPE_INVALID;
        }
        symbol->used = true;
        expr->as.ident.symbol = symbol;
        return symbol->type;
    }
    case EXPR_UNARY:
        return check_unary(c, expr, hint);
    case EXPR_BINARY:
        return check_binary(c, expr, hint);
    case EXPR_CALL:
        return check_call(c, expr);
    case EXPR_CAST:
        return check_cast(c, expr);
    case EXPR_SIZEOF: {
        /* A size is known here, so the node becomes an integer literal: it
           then adapts to its context like any other, and the later passes
           never see a sizeof. */
        TypeKind of = expr->as.size_of.type;
        if (expr->as.size_of.operand) {
            of = check_expr(c, expr->as.size_of.operand, TYPE_INVALID);
        }
        if (of == TYPE_VOID) error(c, expr->loc, "cannot take the size of 'void'");
        expr->kind = EXPR_INT_LIT;
        expr->as.int_lit = type_size(of);
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

/* True if the value is known at compile time. */
static bool is_constant(const Expr *expr) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_BOOL_LIT:
        return true;
    case EXPR_IDENT:
        return !expr->as.ident.symbol || expr->as.ident.symbol->kind == SYM_CONST;
    case EXPR_UNARY:
        return is_constant(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_constant(expr->as.binary.left) && is_constant(expr->as.binary.right);
    case EXPR_CAST:
        return is_constant(expr->as.cast.operand);
    case EXPR_CALL:
    case EXPR_SIZEOF:
        return false;
    }
    return false;
}

/* ---- Statements ---- */

static void check_block(Checker *c, Block *block);

static void check_decl(Checker *c, Stmt *stmt) {
    TypeKind type = stmt->as.decl.type;
    Expr *init = stmt->as.decl.init;
    bool is_const = stmt->kind == STMT_CONST;

    /* The initializer is checked before the name is declared, so a
       declaration cannot refer to itself. */
    if (init) {
        TypeKind found = check_expr(c, init, type);
        if (found != TYPE_INVALID && found != type) {
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
    stmt->as.decl.symbol = symbol;
    declare(c, symbol);
    if (!is_const) {
        list_push(c, symbol->global ? &c->unit->program.globals : c->locals, symbol);
    }
}

static void check_assign(Checker *c, Stmt *stmt) {
    Str name = stmt->as.assign.name;
    Symbol *symbol = resolve(c, name, stmt->loc);
    TypeKind expected = TYPE_INVALID;
    if (symbol && symbol->kind != SYM_VAR) {
        const char *what = symbol->kind == SYM_CONST   ? "constant"
                           : symbol->kind == SYM_PARAM ? "parameter"
                                                       : "function";
        error(c, stmt->loc, "cannot assign to %s '%.*s'", what, NAME(name));
    } else if (symbol) {
        stmt->as.assign.symbol = symbol;
        expected = symbol->type;
    }
    TypeKind found = check_expr(c, stmt->as.assign.value, expected);
    if (expected == TYPE_INVALID) return;

    Op op = stmt->as.assign.op;
    if (stmt->as.assign.compound &&
        !(op == OP_MOD ? type_is_integer(expected) : type_is_numeric(expected))) {
        error(c, stmt->loc, "operator '%s=' is not defined for '%s'", op_text(op),
              type_name(expected));
    } else if (found != TYPE_INVALID && found != expected) {
        error(c, stmt->as.assign.value->loc, "cannot assign '%s' to '%s' variable '%.*s'",
              type_name(found), type_name(expected), NAME(name));
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

/* Checks a block's statements in a scope of their own. */
static void check_block(Checker *c, Block *block) {
    Scope scope;
    push_scope(c, &scope, block->close.offset);
    for (size_t i = 0; i < block->count; i++) check_stmt(c, block->stmts[i]);
    pop_scope(c);
}

/* Labels are declared up front so that a goto can jump forward. They belong
   to the whole function, whatever block they sit in. */
static void collect_labels(Checker *c, Block *block) {
    for (size_t i = 0; i < block->count; i++) {
        Stmt *stmt = block->stmts[i];
        if (stmt->kind == STMT_IF) {
            collect_labels(c, &stmt->as.if_.then_block);
            if (stmt->as.if_.has_else) collect_labels(c, &stmt->as.if_.else_block);
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

/* True if control cannot run off the end of the block. Conservative: it only
   recognizes a final return or goto, and an if/else whose branches both
   qualify. */
static bool block_terminates(const Block *block) {
    if (block->count == 0) return false;
    const Stmt *last = block->stmts[block->count - 1];
    switch (last->kind) {
    case STMT_RETURN:
    case STMT_GOTO:
        return true;
    case STMT_IF:
        return last->as.if_.has_else && block_terminates(&last->as.if_.then_block) &&
               block_terminates(&last->as.if_.else_block);
    default:
        return false;
    }
}

static void check_func(Checker *c, Stmt *stmt) {
    Stmt *outer_func = c->func;
    SymbolList *outer_locals = c->locals;
    SymbolList outer_labels = c->labels;
    Scope *outer_scope = c->scope;

    /* A function body sees the top level only. */
    c->scope = c->global;
    c->func = stmt;
    c->locals = &stmt->as.func.locals;
    memset(&c->labels, 0, sizeof c->labels);

    Block *body = &stmt->as.func.body;
    Scope scope;
    push_scope(c, &scope, body->close.offset);
    for (size_t i = 0; i < stmt->as.func.param_count; i++) {
        Param *param = &stmt->as.func.params[i];
        Symbol *symbol = new_symbol(c, SYM_PARAM, param->name, param->loc, param->type);
        symbol->visible_from = body->open.offset;
        param->symbol = symbol;
        declare(c, symbol);
    }
    collect_labels(c, body);
    for (size_t i = 0; i < body->count; i++) check_stmt(c, body->stmts[i]);
    pop_scope(c);

    if (stmt->as.func.ret != TYPE_VOID && !block_terminates(body)) {
        error(c, stmt->loc, "'%.*s' can reach its end without returning a '%s' value",
              NAME(stmt->as.func.name), type_name(stmt->as.func.ret));
    }

    c->func = outer_func;
    c->locals = outer_locals;
    c->labels = outer_labels;
    c->scope = outer_scope;
}

static void check_stmt(Checker *c, Stmt *stmt) {
    switch (stmt->kind) {
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
            if (check_expr(c, arg, TYPE_INVALID) == TYPE_VOID) {
                error(c, arg->loc, "cannot print a 'void' value");
            }
        }
        break;
    case STMT_IF: {
        Expr *cond = stmt->as.if_.cond;
        TypeKind type = check_expr(c, cond, TYPE_INVALID);
        if (type != TYPE_BOOL && type != TYPE_INVALID) {
            error(c, cond->loc, "condition must be 'bool', found '%s'", type_name(type));
        }
        check_block(c, &stmt->as.if_.then_block);
        if (stmt->as.if_.has_else) check_block(c, &stmt->as.if_.else_block);
        break;
    }
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
    }
}

static void warn_unused(Checker *c) {
    for (size_t i = 0; i < c->unit->symbols.count; i++) {
        const Symbol *symbol = c->unit->symbols.items[i];
        const char *what;
        switch (symbol->kind) {
        case SYM_CONST: what = "constant"; break;
        case SYM_VAR:   what = "variable"; break;
        case SYM_FUNC:  what = "function"; break;
        case SYM_LABEL: what = "label";    break;
        default:        continue;
        }
        /* main is called by the runtime. */
        if (symbol->decl && symbol->decl == c->unit->program.main) continue;
        if (!symbol->used) {
            diag_add(c->diags, SEV_WARNING, symbol->loc, "%s '%.*s' is never used", what,
                     NAME(symbol->name));
        }
    }
}

/* A file that defines main is laid out like a C program: declarations at the
   top level, and main as the entry point. */
static void check_main(Checker *c, const Stmt *main) {
    c->unit->program.main = main;
    if (main->as.func.param_count != 0) {
        error(c, main->loc, "'main' cannot take parameters");
    }
    if (main->as.func.ret != TYPE_INT && main->as.func.ret != TYPE_VOID) {
        error(c, main->loc, "'main' must return 'int' or 'void', not '%s'",
              type_name(main->as.func.ret));
    }
    const Block *top = &c->unit->program.top;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *stmt = top->stmts[i];
        if (stmt->kind != STMT_CONST && stmt->kind != STMT_VAR && stmt->kind != STMT_FUNC) {
            error(c, stmt->loc,
                  "a file that defines 'main' can only have declarations at the top "
                  "level; move this statement into a function");
        }
    }
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

    /* Functions are declared before anything is checked, so they can be
       called from code above them and from each other. */
    for (size_t i = 0; i < top->count; i++) {
        Stmt *stmt = top->stmts[i];
        if (stmt->kind != STMT_FUNC) continue;
        Symbol *symbol = new_symbol(&c, SYM_FUNC, stmt->as.func.name, stmt->loc,
                                    stmt->as.func.ret);
        symbol->decl = stmt;
        symbol->visible_from = 0;
        stmt->as.func.symbol = symbol;
        declare(&c, symbol);
    }

    const Symbol *main_symbol = find_in(&global.symbols, (Str){"main", 4});
    if (main_symbol) check_main(&c, main_symbol->decl);

    collect_labels(&c, top);
    for (size_t i = 0; i < top->count; i++) check_stmt(&c, top->stmts[i]);
    warn_unused(&c);
}

void unit_analyze(Unit *unit, Str src) {
    memset(unit, 0, sizeof *unit);
    unit->src = src;
    unit->diags.arena = &unit->arena;
    unit->program = parse_program(src, &unit->arena, &unit->diags);
    check_program(unit);
    diags_sort(&unit->diags);
}

void unit_release(Unit *unit) {
    arena_release(&unit->arena);
    memset(unit, 0, sizeof *unit);
}
