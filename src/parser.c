#include "ast.h"
#include "lexer.h"

/*
 * Grammar:
 *
 *   program    = { statement terminator } EOF
 *   terminator = NEWLINE | ";" | EOF
 *   statement  = const_decl | cout_stmt
 *   const_decl = "const" type IDENT "=" expr
 *   cout_stmt  = "cout" "<<" expr { "<<" expr }
 *   type       = "string"
 *   expr       = STRING | IDENT
 *
 * A newline directly after "=" or "<<" does not end the statement.
 *
 * Error handling: a parse function that fails reports one diagnostic and
 * returns NULL (or false). parse_program then drops the statement and
 * resynchronizes at the next statement boundary.
 */

typedef struct {
    Lexer lexer;
    Token tok;
    Arena *arena;
    Diags *diags;
} Parser;

static void next(Parser *p) {
    p->tok = lexer_next(&p->lexer);
}

static void skip_newlines(Parser *p) {
    while (p->tok.kind == TOK_NEWLINE) next(p);
}

static void error_expected(Parser *p, const char *what) {
    diag_add(p->diags, SEV_ERROR, p->tok.loc, "expected %s, found %s", what,
             token_kind_name(p->tok.kind));
}

static bool expect(Parser *p, TokenKind kind, Token *out) {
    if (p->tok.kind != kind) {
        error_expected(p, token_kind_name(kind));
        return false;
    }
    if (out) *out = p->tok;
    next(p);
    return true;
}

static bool parse_type(Parser *p, TypeKind *out) {
    if (p->tok.kind != TOK_KW_STRING) {
        error_expected(p, "a type");
        return false;
    }
    next(p);
    *out = TYPE_STRING;
    return true;
}

static Expr *parse_expr(Parser *p) {
    if (p->tok.kind != TOK_STRING && p->tok.kind != TOK_IDENT) {
        error_expected(p, "an expression");
        return NULL;
    }
    Expr *expr = arena_alloc(p->arena, sizeof *expr);
    expr->loc = p->tok.loc;
    expr->type = TYPE_INVALID;
    if (p->tok.kind == TOK_STRING) {
        expr->kind = EXPR_STRING_LIT;
        expr->as.string_lit = p->tok.text;
    } else {
        expr->kind = EXPR_IDENT;
        expr->as.ident.name = p->tok.text;
        expr->as.ident.decl = NULL;
    }
    next(p);
    return expr;
}

static Stmt *parse_const_decl(Parser *p) {
    TypeKind type;
    Token name;
    if (!expect(p, TOK_KW_CONST, NULL)) return NULL;
    if (!parse_type(p, &type)) return NULL;
    if (!expect(p, TOK_IDENT, &name)) return NULL;
    if (!expect(p, TOK_ASSIGN, NULL)) return NULL;
    skip_newlines(p);
    Expr *init = parse_expr(p);
    if (!init) return NULL;

    Stmt *stmt = arena_alloc(p->arena, sizeof *stmt);
    stmt->kind = STMT_CONST_DECL;
    stmt->loc = name.loc;
    stmt->as.const_decl.type = type;
    stmt->as.const_decl.name = name.text;
    stmt->as.const_decl.init = init;
    stmt->as.const_decl.used = false;
    return stmt;
}

static Stmt *parse_cout(Parser *p) {
    Token keyword;
    if (!expect(p, TOK_KW_COUT, &keyword)) return NULL;

    Expr **args = NULL;
    size_t count = 0, cap = 0;
    do {
        if (!expect(p, TOK_SHL, NULL)) return NULL;
        skip_newlines(p);
        Expr *arg = parse_expr(p);
        if (!arg) return NULL;
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            args = arena_grow(p->arena, args, count * sizeof *args,
                              new_cap * sizeof *args);
            cap = new_cap;
        }
        args[count++] = arg;
    } while (p->tok.kind == TOK_SHL);

    Stmt *stmt = arena_alloc(p->arena, sizeof *stmt);
    stmt->kind = STMT_COUT;
    stmt->loc = keyword.loc;
    stmt->as.cout.args = args;
    stmt->as.cout.count = count;
    return stmt;
}

static bool at_statement_start(const Parser *p) {
    return p->tok.kind == TOK_KW_CONST || p->tok.kind == TOK_KW_COUT;
}

static Stmt *parse_statement(Parser *p) {
    switch (p->tok.kind) {
    case TOK_KW_CONST: return parse_const_decl(p);
    case TOK_KW_COUT:  return parse_cout(p);
    default:
        error_expected(p, "a statement");
        /* Consumed so that resynchronizing always makes progress. */
        next(p);
        return NULL;
    }
}

static bool at_terminator(const Parser *p) {
    return p->tok.kind == TOK_NEWLINE || p->tok.kind == TOK_SEMI ||
           p->tok.kind == TOK_EOF;
}

/* Skips the rest of a broken statement. Stopping at a statement keyword keeps
   a following statement intact when the broken one swallowed its newline. */
static void synchronize(Parser *p) {
    while (!at_terminator(p) && !at_statement_start(p)) next(p);
}

static void skip_terminators(Parser *p) {
    while (p->tok.kind == TOK_NEWLINE || p->tok.kind == TOK_SEMI) next(p);
}

Program parse_program(Str src, Arena *arena, Diags *diags) {
    Parser p = {.arena = arena, .diags = diags};
    lexer_init(&p.lexer, src, arena, diags);
    next(&p);

    Program program = {0};
    size_t cap = 0;
    skip_terminators(&p);
    while (p.tok.kind != TOK_EOF) {
        Stmt *stmt = parse_statement(&p);
        if (!stmt) {
            synchronize(&p);
        } else {
            if (!at_terminator(&p)) {
                error_expected(&p, "end of statement");
                synchronize(&p);
            }
            if (program.count == cap) {
                size_t new_cap = cap ? cap * 2 : 16;
                program.stmts =
                    arena_grow(arena, program.stmts,
                               program.count * sizeof *program.stmts,
                               new_cap * sizeof *program.stmts);
                cap = new_cap;
            }
            program.stmts[program.count++] = stmt;
        }
        skip_terminators(&p);
    }
    return program;
}
