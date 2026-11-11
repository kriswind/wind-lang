#include "ast.h"

typedef struct {
    Arena *arena;
    Diags *diags;
    Stmt **decls;
    size_t count;
    size_t cap;
} Checker;

const char *type_name(TypeKind type) {
    switch (type) {
    case TYPE_INVALID: return "<invalid>";
    case TYPE_STRING:  return "string";
    }
    return "?";
}

/* Terminates because a constant can only refer to earlier declarations. */
const Expr *const_decl_literal(const Stmt *decl) {
    const Expr *expr = decl->as.const_decl.init;
    while (expr->kind == EXPR_IDENT) {
        if (!expr->as.ident.decl) return NULL;
        expr = expr->as.ident.decl->as.const_decl.init;
    }
    return expr;
}

static Stmt *lookup(const Checker *c, Str name) {
    for (size_t i = 0; i < c->count; i++) {
        if (str_eq(c->decls[i]->as.const_decl.name, name)) return c->decls[i];
    }
    return NULL;
}

static void declare(Checker *c, Stmt *decl) {
    Str name = decl->as.const_decl.name;
    const Stmt *prev = lookup(c, name);
    if (prev) {
        diag_add(c->diags, SEV_ERROR, decl->loc,
                 "'%.*s' is already defined at line %d", (int)name.len,
                 name.data, prev->loc.line);
        return;
    }
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        c->decls = arena_grow(c->arena, c->decls, c->count * sizeof *c->decls,
                              cap * sizeof *c->decls);
        c->cap = cap;
    }
    c->decls[c->count++] = decl;
}

static void check_expr(Checker *c, Expr *expr) {
    switch (expr->kind) {
    case EXPR_STRING_LIT:
        expr->type = TYPE_STRING;
        break;
    case EXPR_IDENT: {
        Str name = expr->as.ident.name;
        Stmt *decl = lookup(c, name);
        if (!decl) {
            diag_add(c->diags, SEV_ERROR, expr->loc, "undefined name '%.*s'",
                     (int)name.len, name.data);
            expr->type = TYPE_INVALID;
            break;
        }
        decl->as.const_decl.used = true;
        expr->as.ident.decl = decl;
        expr->type = decl->as.const_decl.type;
        break;
    }
    }
}

void check_program(Program *program, Arena *arena, Diags *diags) {
    Checker c = {.arena = arena, .diags = diags};

    for (size_t i = 0; i < program->count; i++) {
        Stmt *stmt = program->stmts[i];
        switch (stmt->kind) {
        case STMT_CONST_DECL: {
            Expr *init = stmt->as.const_decl.init;
            /* The initializer is checked before the name is declared, so a
               constant cannot refer to itself. */
            check_expr(&c, init);
            if (init->type != TYPE_INVALID &&
                init->type != stmt->as.const_decl.type) {
                diag_add(diags, SEV_ERROR, init->loc,
                         "cannot initialize '%s' constant with '%s'",
                         type_name(stmt->as.const_decl.type),
                         type_name(init->type));
            }
            declare(&c, stmt);
            break;
        }
        case STMT_COUT:
            for (size_t j = 0; j < stmt->as.cout.count; j++) {
                check_expr(&c, stmt->as.cout.args[j]);
            }
            break;
        }
    }

    for (size_t i = 0; i < c.count; i++) {
        const Stmt *decl = c.decls[i];
        if (!decl->as.const_decl.used) {
            Str name = decl->as.const_decl.name;
            diag_add(diags, SEV_WARNING, decl->loc,
                     "constant '%.*s' is never used", (int)name.len, name.data);
        }
    }
}

void unit_analyze(Unit *unit, Str src) {
    *unit = (Unit){.src = src};
    unit->diags.arena = &unit->arena;
    unit->program = parse_program(src, &unit->arena, &unit->diags);
    check_program(&unit->program, &unit->arena, &unit->diags);
    diags_sort(&unit->diags);
}

void unit_release(Unit *unit) {
    arena_release(&unit->arena);
    *unit = (Unit){0};
}
