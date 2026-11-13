#include <string.h>

#include "ast.h"
#include "lexer.h"

/*
 * Grammar:
 *
 *   program    = { statement } EOF
 *   block      = "{" { statement } "}"
 *   statement  = ( const_decl | var_decl | assign | call | cout | cin | goto
 *                | return | do_while | "break" | "continue" | import
 *                | "export" const_decl ) terminator
 *              | if | while | for | [ "export" ] func | label
 *   import     = "import" STRING [ "as" IDENT ]
 *   terminator = NEWLINE | ";" | EOF | before "}"
 *
 *   const_decl = "const" type IDENT "=" expr
 *   var_decl   = type IDENT [ "=" expr ]
 *              | type IDENT "[" [ expr ] "]" [ "=" "{" [ expr { "," expr } ] "}" ]
 *   assign     = place ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|="
 *                      | "^=" | "<<=" | ">>=" ) expr
 *              | place ( "++" | "--" ) | ( "++" | "--" ) place
 *   place      = IDENT [ "[" expr "]" ]
 *   cout       = "cout" "<<" expr { "<<" expr }
 *   cin        = "cin" ">>" target { ">>" target }
 *   target     = type IDENT | place
 *   goto       = "goto" IDENT
 *   label      = IDENT ":"
 *   return     = "return" [ expr ]
 *   if         = "if" expr block [ "else" body ]
 *              | "if" "(" expr ")" body [ "else" body ]
 *   while      = "while" expr block | "while" "(" expr ")" body
 *   do_while   = "do" body "while" expr
 *   for        = "for" "(" clauses ")" body | "for" clauses block
 *   body       = block | statement
 *   clauses    = [ simple ] ";" [ expr ] ";" [ simple ]
 *   simple     = var_decl | assign | call
 *   func       = type IDENT "(" [ "void" | param { "," param } ] ")" block
 *   param      = type IDENT [ "[" "]" ]
 *   type       = TYPE | a C spelling such as "unsigned long" (see parse_type)
 *
 *   expr       = or [ "?" expr ":" expr ]
 *   or         = and { "||" and }
 *   and        = compare { "&&" compare }
 *   compare    = bit_or { ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) bit_or }
 *   bit_or     = bit_xor { "|" bit_xor }
 *   bit_xor    = bit_and { "^" bit_and }
 *   bit_and    = shift { "&" shift }
 *   shift      = sum { ( "<<" | ">>" ) sum }
 *   sum        = term { ( "+" | "-" ) term }
 *   term       = unary { ( "*" | "/" | "%" ) unary }
 *   unary      = ( "-" | "!" | "~" ) unary | postfix
 *   postfix    = primary { "[" expr "]" }
 *   primary    = INT | FLOAT | CHAR | STRING | "true" | "false" | name
 *              | call | type "(" expr ")" | "(" type ")" unary | "(" expr ")"
 *              | "sizeof" "(" type ")" | "sizeof" unary | "cin"
 *   call       = name "(" [ expr { "," expr } ] ")"
 *   name       = IDENT | IDENT "." IDENT
 *
 * In a cout statement "<<" separates the values, so a shift there must be
 * inside parentheses (or brackets, or a call's arguments).
 *
 * A body without braces holds one statement, which may not be a declaration
 * or a label. It is only allowed where the header before it ends visibly: at
 * a ")" that closes the whole condition, or after "else" or "do".
 *
 * "as" is only special after an import's path, and is otherwise a name.
 * "IDENT.IDENT" names something that an imported file exports.
 *
 * Functions, imports and "export" may only appear at the top level. A newline does not end a
 * statement directly after "=", "<<", ">>", "," or a binary operator, inside
 * parentheses, before or after the "?" and ":" of a conditional, or between a
 * condition or "}" and what follows it.
 *
 * Error handling: a parse function that fails reports one diagnostic and
 * returns NULL (or false). The statement is dropped and parsing resumes at
 * the next statement boundary.
 */

typedef struct {
    Lexer lexer;
    Token tok;
    SrcLoc prev; /* the most recently consumed token */
    /* Set while parsing the values of a cout statement, where "<<" and ">>"
       are not operators. */
    bool no_shift;
    Arena *arena;
    Diags *diags;
} Parser;

static void next(Parser *p) {
    p->prev = p->tok.loc;
    p->tok = lexer_next(&p->lexer);
}

static bool at(const Parser *p, TokenKind kind) {
    return p->tok.kind == kind;
}

static bool accept(Parser *p, TokenKind kind) {
    if (!at(p, kind)) return false;
    next(p);
    return true;
}

static void skip_newlines(Parser *p) {
    while (at(p, TOK_NEWLINE)) next(p);
}

static void error_expected(Parser *p, const char *what) {
    diag_add(p->diags, SEV_ERROR, p->tok.loc, "expected %s, found %s", what,
             token_kind_name(p->tok.kind));
}

static bool expect(Parser *p, TokenKind kind, Token *out) {
    if (!at(p, kind)) {
        error_expected(p, token_kind_name(kind));
        return false;
    }
    if (out) *out = p->tok;
    next(p);
    return true;
}

/* The region from the start of one token to the end of another, cut down to
   the first token when they are on different lines. */
static SrcLoc span(SrcLoc start, SrcLoc end) {
    if (end.line == start.line) start.len = end.offset + end.len - start.offset;
    return start;
}

static Expr *new_expr(Parser *p, ExprKind kind, SrcLoc loc) {
    Expr *expr = arena_alloc(p->arena, sizeof *expr);
    memset(expr, 0, sizeof *expr);
    expr->kind = kind;
    expr->loc = loc;
    expr->type = TYPE_INVALID;
    return expr;
}

static Stmt *new_stmt(Parser *p, StmtKind kind, SrcLoc loc) {
    Stmt *stmt = arena_alloc(p->arena, sizeof *stmt);
    memset(stmt, 0, sizeof *stmt);
    stmt->kind = kind;
    stmt->loc = loc;
    return stmt;
}

/* ---- Types ---- */

static bool at_type(const Parser *p) {
    return at(p, TOK_TYPE) || at(p, TOK_KW_UNSIGNED) || at(p, TOK_KW_SIGNED) ||
           at(p, TOK_KW_LONG) || at(p, TOK_KW_SHORT);
}

/*
 * Parses a type name. Besides Wind's own names this accepts C's multi-word
 * spellings and maps them onto Wind types:
 *
 *   short, short int                   i16     unsigned short       u16
 *   long, long int, long long          i64     unsigned long (long) u64
 *   signed, signed int                 int     unsigned (int)       uint
 *   signed char                        i8      unsigned char        u8
 *   long double                        f128
 */
static bool parse_type(Parser *p, TypeKind *out) {
    SrcLoc start = p->tok.loc;
    bool is_unsigned = false, is_signed = false;
    int shorts = 0, longs = 0;
    for (;;) {
        if (accept(p, TOK_KW_UNSIGNED)) is_unsigned = true;
        else if (accept(p, TOK_KW_SIGNED)) is_signed = true;
        else if (accept(p, TOK_KW_SHORT)) shorts++;
        else if (accept(p, TOK_KW_LONG)) longs++;
        else break;
    }
    bool modified = is_unsigned || is_signed || shorts || longs;
    if (!modified) {
        Token type;
        if (!at(p, TOK_TYPE)) {
            error_expected(p, "a type");
            return false;
        }
        type = p->tok;
        next(p);
        *out = type.type;
        return true;
    }

    TypeKind base = TYPE_INT;
    if (at(p, TOK_TYPE)) {
        base = p->tok.type;
        next(p);
    }
    bool valid = !(is_unsigned && is_signed) && shorts <= 1 && longs <= 2 &&
                 !(shorts && longs);
    if (base == TYPE_INT) {
        if (shorts) *out = is_unsigned ? TYPE_U16 : TYPE_I16;
        else if (longs) *out = is_unsigned ? TYPE_U64 : TYPE_I64;
        else *out = is_unsigned ? TYPE_UINT : TYPE_INT;
    } else if (base == TYPE_CHAR && !shorts && !longs) {
        *out = is_unsigned ? TYPE_U8 : TYPE_I8;
    } else if (base == TYPE_F64 && longs == 1 && !shorts && !is_unsigned && !is_signed) {
        *out = TYPE_F128;
    } else {
        valid = false;
    }
    if (!valid) {
        diag_add(p->diags, SEV_ERROR, span(start, p->prev), "invalid type");
        return false;
    }
    return true;
}

/* ---- Expressions ---- */

static Expr *parse_expr(Parser *p);
static Expr *parse_unary(Parser *p);

/* Parses an expression enclosed by brackets of some kind, inside which
   shifts are operators again even within a cout statement. */
static Expr *parse_enclosed_expr(Parser *p) {
    bool saved = p->no_shift;
    p->no_shift = false;
    Expr *expr = parse_expr(p);
    p->no_shift = saved;
    return expr;
}

/* Called at "sizeof". Its operand is a parenthesized type or an expression. */
static Expr *parse_sizeof(Parser *p) {
    Token tok = p->tok;
    Expr *expr = new_expr(p, EXPR_SIZEOF, tok.loc);
    next(p);
    if (accept(p, TOK_LPAREN)) {
        skip_newlines(p);
        if (at_type(p)) {
            if (!parse_type(p, &expr->as.size_of.type)) return NULL;
        } else {
            expr->as.size_of.operand = parse_enclosed_expr(p);
            if (!expr->as.size_of.operand) return NULL;
        }
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return NULL;
    } else {
        expr->as.size_of.operand = parse_unary(p);
        if (!expr->as.size_of.operand) return NULL;
    }
    expr->loc = span(tok.loc, p->prev);
    return expr;
}

/* Called at the "(" that follows the callee's name. */
static Expr *parse_call(Parser *p, Token name) {
    Expr *call = new_expr(p, EXPR_CALL, name.loc);
    call->as.call.name = name.text;
    call->as.call.name_loc = name.loc;
    size_t cap = 0;

    next(p);
    skip_newlines(p);
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            Expr *arg = parse_enclosed_expr(p);
            if (!arg) return NULL;
            ARENA_PUSH(p->arena, call->as.call.args, call->as.call.count, cap, arg);
            skip_newlines(p);
            if (!accept(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
    }
    if (!expect(p, TOK_RPAREN, NULL)) return NULL;
    call->loc = span(name.loc, p->prev);
    return call;
}

/* Called at the "." that follows a module's name: parses the name taken
   from the module, and the call if it is one. */
static Expr *parse_member(Parser *p, Token module) {
    Token name;
    next(p);
    if (!expect(p, TOK_IDENT, &name)) return NULL;
    if (at(p, TOK_LPAREN)) {
        Expr *call = parse_call(p, name);
        if (!call) return NULL;
        call->as.call.module = module.text;
        call->as.call.module_loc = module.loc;
        call->loc = span(module.loc, p->prev);
        return call;
    }
    Expr *expr = new_expr(p, EXPR_IDENT, span(module.loc, name.loc));
    expr->as.ident.name = name.text;
    expr->as.ident.name_loc = name.loc;
    expr->as.ident.module = module.text;
    expr->as.ident.module_loc = module.loc;
    return expr;
}

/* True if the next token that is not a newline is of the given kind, which
   lets an expression carry on from the start of a following line. Looks
   ahead without consuming anything. */
static bool continues_with(Parser *p, TokenKind kind) {
    if (!at(p, TOK_NEWLINE)) return at(p, kind);
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    Token tok;
    do tok = lexer_next(&lexer);
    while (tok.kind == TOK_NEWLINE);
    /* Problems found while looking ahead are dropped here; they are
       reported when the tokens are parsed. */
    p->diags->count = count;
    p->diags->error_count = error_count;
    return tok.kind == kind;
}

/* True at a "(" that opens a C-style cast: one holding a type and nothing
   else. Looks ahead without consuming anything. */
static bool at_c_cast(Parser *p) {
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    bool has_type = false;
    Token tok = lexer_next(&lexer);
    while (tok.kind == TOK_TYPE || tok.kind == TOK_KW_UNSIGNED ||
           tok.kind == TOK_KW_SIGNED || tok.kind == TOK_KW_LONG ||
           tok.kind == TOK_KW_SHORT) {
        has_type = true;
        tok = lexer_next(&lexer);
    }
    /* Problems found while looking ahead are dropped here; they are
       reported when the tokens are parsed. */
    p->diags->count = count;
    p->diags->error_count = error_count;
    return has_type && tok.kind == TOK_RPAREN;
}

static Expr *parse_primary(Parser *p) {
    Token tok = p->tok;
    Expr *expr;
    if (at_type(p)) {
        TypeKind to;
        if (!parse_type(p, &to)) return NULL;
        if (!expect(p, TOK_LPAREN, NULL)) return NULL;
        skip_newlines(p);
        Expr *operand = parse_enclosed_expr(p);
        if (!operand) return NULL;
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return NULL;
        expr = new_expr(p, EXPR_CAST, span(tok.loc, p->prev));
        expr->as.cast.to = to;
        expr->as.cast.operand = operand;
        return expr;
    }
    switch (tok.kind) {
    case TOK_KW_SIZEOF:
        return parse_sizeof(p);
    case TOK_KW_CIN:
        next(p);
        return new_expr(p, EXPR_CIN_OK, tok.loc);
    case TOK_INT:
        next(p);
        expr = new_expr(p, EXPR_INT_LIT, tok.loc);
        expr->as.int_lit = tok.int_value;
        return expr;
    case TOK_FLOAT:
        next(p);
        expr = new_expr(p, EXPR_FLOAT_LIT, tok.loc);
        expr->as.float_lit = tok.text;
        return expr;
    case TOK_CHAR:
        next(p);
        expr = new_expr(p, EXPR_CHAR_LIT, tok.loc);
        expr->as.char_lit = (char)tok.int_value;
        return expr;
    case TOK_STRING:
        next(p);
        expr = new_expr(p, EXPR_STRING_LIT, tok.loc);
        expr->as.string_lit = tok.text;
        return expr;
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
        next(p);
        expr = new_expr(p, EXPR_BOOL_LIT, tok.loc);
        expr->as.bool_lit = tok.kind == TOK_KW_TRUE;
        return expr;
    case TOK_IDENT:
        next(p);
        if (at(p, TOK_LPAREN)) return parse_call(p, tok);
        if (at(p, TOK_DOT)) return parse_member(p, tok);
        expr = new_expr(p, EXPR_IDENT, tok.loc);
        expr->as.ident.name = tok.text;
        expr->as.ident.name_loc = tok.loc;
        return expr;
    case TOK_LPAREN:
        if (at_c_cast(p)) {
            TypeKind to;
            next(p);
            if (!parse_type(p, &to)) return NULL;
            if (!expect(p, TOK_RPAREN, NULL)) return NULL;
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            expr = new_expr(p, EXPR_CAST, span(tok.loc, operand->loc));
            expr->as.cast.to = to;
            expr->as.cast.operand = operand;
            return expr;
        }
        next(p);
        skip_newlines(p);
        expr = parse_enclosed_expr(p);
        if (!expr) return NULL;
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return NULL;
        expr->loc = span(tok.loc, p->prev);
        expr->parenthesized = true;
        return expr;
    default:
        error_expected(p, "an expression");
        return NULL;
    }
}

/* Called at the "[" that follows the indexed expression. */
static Expr *parse_index(Parser *p, Expr *base) {
    next(p);
    skip_newlines(p);
    Expr *index = parse_enclosed_expr(p);
    if (!index) return NULL;
    skip_newlines(p);
    if (!expect(p, TOK_RBRACKET, NULL)) return NULL;
    Expr *expr = new_expr(p, EXPR_INDEX, span(base->loc, p->prev));
    expr->as.index.base = base;
    expr->as.index.index = index;
    return expr;
}

static Expr *parse_postfix(Parser *p) {
    Expr *expr = parse_primary(p);
    while (expr && at(p, TOK_LBRACKET)) expr = parse_index(p, expr);
    return expr;
}

static Expr *parse_unary(Parser *p) {
    if (!at(p, TOK_MINUS) && !at(p, TOK_NOT) && !at(p, TOK_TILDE)) return parse_postfix(p);
    Token tok = p->tok;
    next(p);
    Expr *operand = parse_unary(p);
    if (!operand) return NULL;
    Expr *expr = new_expr(p, EXPR_UNARY, span(tok.loc, operand->loc));
    expr->as.unary.op = tok.kind == TOK_MINUS ? OP_NEG
                        : tok.kind == TOK_NOT ? OP_NOT
                                              : OP_BIT_NOT;
    expr->as.unary.operand = operand;
    return expr;
}

/* Returns the binding strength of a binary operator token, or 0 if the token
   is not one. Higher binds tighter. Unlike in C, the bitwise operators bind
   tighter than comparisons, so "x & 1 == 0" compares the masked value. */
static int binary_precedence(const Parser *p, Op *op) {
    switch (p->tok.kind) {
    case TOK_OR:      *op = OP_OR;      return 1;
    case TOK_AND:     *op = OP_AND;     return 2;
    case TOK_EQ:      *op = OP_EQ;      return 3;
    case TOK_NE:      *op = OP_NE;      return 3;
    case TOK_LT:      *op = OP_LT;      return 3;
    case TOK_LE:      *op = OP_LE;      return 3;
    case TOK_GT:      *op = OP_GT;      return 3;
    case TOK_GE:      *op = OP_GE;      return 3;
    case TOK_PIPE:    *op = OP_BIT_OR;  return 4;
    case TOK_CARET:   *op = OP_BIT_XOR; return 5;
    case TOK_AMP:     *op = OP_BIT_AND; return 6;
    case TOK_SHL:     *op = OP_SHL;     return p->no_shift ? 0 : 7;
    case TOK_SHR:     *op = OP_SHR;     return p->no_shift ? 0 : 7;
    case TOK_PLUS:    *op = OP_ADD;     return 8;
    case TOK_MINUS:   *op = OP_SUB;     return 8;
    case TOK_STAR:    *op = OP_MUL;     return 9;
    case TOK_SLASH:   *op = OP_DIV;     return 9;
    case TOK_PERCENT: *op = OP_MOD;     return 9;
    default:          return 0;
    }
}

static Expr *parse_binary(Parser *p, int min_precedence) {
    Expr *left = parse_unary(p);
    if (!left) return NULL;
    for (;;) {
        Op op;
        int precedence = binary_precedence(p, &op);
        if (precedence == 0 || precedence < min_precedence) return left;
        next(p);
        skip_newlines(p);
        Expr *right = parse_binary(p, precedence + 1);
        if (!right) return NULL;
        Expr *expr = new_expr(p, EXPR_BINARY, span(left->loc, right->loc));
        expr->as.binary.op = op;
        expr->as.binary.left = left;
        expr->as.binary.right = right;
        left = expr;
    }
}

static Expr *parse_expr(Parser *p) {
    Expr *cond = parse_binary(p, 1);
    if (!cond || !continues_with(p, TOK_QUESTION)) return cond;
    skip_newlines(p);
    next(p);
    skip_newlines(p);
    Expr *then = parse_expr(p);
    if (!then) return NULL;
    if (continues_with(p, TOK_COLON)) skip_newlines(p);
    if (!expect(p, TOK_COLON, NULL)) return NULL;
    skip_newlines(p);
    Expr *otherwise = parse_expr(p);
    if (!otherwise) return NULL;
    Expr *expr = new_expr(p, EXPR_TERNARY, span(cond->loc, otherwise->loc));
    expr->as.ternary.cond = cond;
    expr->as.ternary.then = then;
    expr->as.ternary.otherwise = otherwise;
    return expr;
}

/* ---- Statements ---- */

static bool parse_block(Parser *p, Block *block);
static Stmt *parse_if(Parser *p);

static bool at_terminator(const Parser *p) {
    return at(p, TOK_NEWLINE) || at(p, TOK_SEMI) || at(p, TOK_EOF) ||
           at(p, TOK_RBRACE);
}

static bool at_statement_keyword(const Parser *p) {
    switch (p->tok.kind) {
    case TOK_KW_CONST:
    case TOK_KW_COUT:
    case TOK_KW_CIN:
    case TOK_KW_IF:
    case TOK_KW_WHILE:
    case TOK_KW_DO:
    case TOK_KW_FOR:
    case TOK_KW_BREAK:
    case TOK_KW_CONTINUE:
    case TOK_KW_GOTO:
    case TOK_KW_RETURN:
    case TOK_KW_IMPORT:
    case TOK_KW_EXPORT:
        return true;
    default:
        return false;
    }
}

/* Called at the "[" of an array declaration, after its type and name. */
static Stmt *parse_array_decl(Parser *p, StmtKind kind, TypeKind type, Token name) {
    Stmt *stmt = new_stmt(p, kind, name.loc);
    stmt->as.decl.type = type;
    stmt->as.decl.name = name.text;
    stmt->as.decl.is_array = true;
    size_t cap = 0;

    next(p);
    if (!at(p, TOK_RBRACKET)) {
        stmt->as.decl.length = parse_expr(p);
        if (!stmt->as.decl.length) return NULL;
    }
    if (!expect(p, TOK_RBRACKET, NULL)) return NULL;

    if (accept(p, TOK_ASSIGN)) {
        skip_newlines(p);
        if (!expect(p, TOK_LBRACE, NULL)) return NULL;
        skip_newlines(p);
        while (!at(p, TOK_RBRACE)) {
            Expr *elem = parse_expr(p);
            if (!elem) return NULL;
            ARENA_PUSH(p->arena, stmt->as.decl.elems, stmt->as.decl.elem_count, cap, elem);
            skip_newlines(p);
            if (!accept(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
        if (!expect(p, TOK_RBRACE, NULL)) return NULL;
    } else if (kind == STMT_CONST || !stmt->as.decl.length) {
        diag_add(p->diags, SEV_ERROR, name.loc,
                 kind == STMT_CONST ? "a constant array needs its elements: '= { ... }'"
                                    : "an array needs a length or its elements");
        return NULL;
    }
    if (type == TYPE_VOID) {
        diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                 (int)name.text.len, name.text.data);
        return NULL;
    }
    return stmt;
}

/* Called after the type and name of a declaration. */
static Stmt *parse_decl_rest(Parser *p, StmtKind kind, TypeKind type, Token name) {
    if (at(p, TOK_LBRACKET)) return parse_array_decl(p, kind, type, name);
    Expr *init = NULL;
    if (kind == STMT_CONST || at(p, TOK_ASSIGN)) {
        if (!expect(p, TOK_ASSIGN, NULL)) return NULL;
        skip_newlines(p);
        init = parse_expr(p);
        if (!init) return NULL;
    }
    if (type == TYPE_VOID) {
        diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                 (int)name.text.len, name.text.data);
        return NULL;
    }
    Stmt *stmt = new_stmt(p, kind, name.loc);
    stmt->as.decl.type = type;
    stmt->as.decl.name = name.text;
    stmt->as.decl.init = init;
    return stmt;
}

/* Called at the "(" that follows the function's return type and name. */
static Stmt *parse_func(Parser *p, TypeKind ret, Token name, bool top_level) {
    Stmt *stmt = new_stmt(p, STMT_FUNC, name.loc);
    stmt->as.func.ret = ret;
    stmt->as.func.name = name.text;
    size_t cap = 0;

    next(p);
    skip_newlines(p);
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            TypeKind type;
            Token param_name;
            if (!parse_type(p, &type)) return NULL;
            /* C's "(void)" for a function without parameters. */
            if (type == TYPE_VOID && stmt->as.func.param_count == 0 &&
                at(p, TOK_RPAREN)) {
                break;
            }
            if (!expect(p, TOK_IDENT, &param_name)) return NULL;
            if (type == TYPE_VOID) {
                diag_add(p->diags, SEV_ERROR, param_name.loc,
                         "'%.*s' cannot have type 'void'", (int)param_name.text.len,
                         param_name.text.data);
                type = TYPE_INVALID;
            }
            Param param = {type, param_name.text, param_name.loc, false, NULL};
            if (accept(p, TOK_LBRACKET)) {
                if (!at(p, TOK_RBRACKET)) {
                    diag_add(p->diags, SEV_ERROR, p->tok.loc,
                             "an array parameter is written '%s %.*s[]', without a "
                             "length",
                             type_name(type), (int)param_name.text.len,
                             param_name.text.data);
                    return NULL;
                }
                next(p);
                param.is_slice = true;
            }
            ARENA_PUSH(p->arena, stmt->as.func.params, stmt->as.func.param_count, cap,
                       param);
            skip_newlines(p);
            if (!accept(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
    }
    if (!expect(p, TOK_RPAREN, NULL)) return NULL;
    if (!parse_block(p, &stmt->as.func.body)) return NULL;

    if (!top_level) {
        diag_add(p->diags, SEV_ERROR, name.loc,
                 "functions can only be declared at the top level");
        return NULL;
    }
    return stmt;
}

static Stmt *parse_cout(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_COUT, p->tok.loc);
    size_t cap = 0;
    next(p);
    bool ok = true;
    p->no_shift = true;
    do {
        Expr *arg = NULL;
        ok = expect(p, TOK_SHL, NULL);
        if (ok) {
            skip_newlines(p);
            arg = parse_expr(p);
            ok = arg != NULL;
        }
        if (ok) ARENA_PUSH(p->arena, stmt->as.cout.args, stmt->as.cout.count, cap, arg);
    } while (ok && at(p, TOK_SHL));
    p->no_shift = false;
    return ok ? stmt : NULL;
}

/* Parses the optional "[index]" of an assignment target. Returns false on
   error; *index is NULL when there is none. */
static bool parse_place_index(Parser *p, Expr **index) {
    *index = NULL;
    if (!accept(p, TOK_LBRACKET)) return true;
    skip_newlines(p);
    *index = parse_enclosed_expr(p);
    if (!*index) return false;
    skip_newlines(p);
    return expect(p, TOK_RBRACKET, NULL);
}

static Stmt *parse_cin(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_CIN, p->tok.loc);
    size_t cap = 0;
    next(p);
    do {
        Token name;
        ReadTarget target = {0};
        if (!expect(p, TOK_SHR, NULL)) return NULL;
        skip_newlines(p);
        if (at(p, TOK_KW_CONST)) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "a constant's value must be known at compile time and cannot be "
                     "read from input");
            /* Consumed so that recovery does not restart at this "const". */
            next(p);
            return NULL;
        }
        if (at_type(p)) {
            target.declares = true;
            if (!parse_type(p, &target.type)) return NULL;
        }
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        if (target.declares && target.type == TYPE_VOID) {
            diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                     (int)name.text.len, name.text.data);
            return NULL;
        }
        target.name = name.text;
        target.loc = name.loc;
        if (!target.declares && !parse_place_index(p, &target.index)) return NULL;
        ARENA_PUSH(p->arena, stmt->as.cin.targets, stmt->as.cin.count, cap, target);
    } while (at(p, TOK_SHR));
    return stmt;
}

static Stmt *parse_statement(Parser *p, bool top_level, bool *needs_terminator);

/* Parses the body of an if or a loop: a block, or one statement that stands
   for a block holding it. braces_optional tells whether the header ends in a
   way that lets a statement follow it. */
static bool parse_body(Parser *p, Block *block, bool braces_optional) {
    skip_newlines(p);
    if (at(p, TOK_LBRACE)) return parse_block(p, block);
    if (!braces_optional) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc,
                 "expected '{', found %s; a body without braces needs its condition "
                 "in parentheses",
                 token_kind_name(p->tok.kind));
        return false;
    }
    if (at(p, TOK_SEMI)) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc, "an empty body is written '{}'");
        return false;
    }

    SrcLoc start = p->tok.loc;
    bool needs_terminator = true;
    Stmt *stmt = parse_statement(p, false, &needs_terminator);
    if (!stmt) return false;
    if (stmt->kind == STMT_CONST || stmt->kind == STMT_VAR || stmt->kind == STMT_LABEL) {
        diag_add(p->diags, SEV_ERROR, start, "a %s cannot be a body without braces",
                 stmt->kind == STMT_LABEL ? "label" : "declaration");
        return false;
    }
    stmt->start = start;
    stmt->end = p->prev;
    if (needs_terminator) {
        if (!at_terminator(p) && !at(p, TOK_KW_ELSE)) {
            error_expected(p, "end of statement");
            return false;
        }
        /* The statement's own ";", after which an "else" or the "while" of
           a do loop may still follow. */
        accept(p, TOK_SEMI);
    }

    block->stmts = arena_alloc(p->arena, sizeof *block->stmts);
    block->stmts[0] = stmt;
    block->count = 1;
    block->open = start;
    block->close = stmt->end;
    return true;
}

static Stmt *parse_if(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_IF, p->tok.loc);
    next(p);
    stmt->as.if_.cond = parse_expr(p);
    if (!stmt->as.if_.cond) return NULL;
    if (!parse_body(p, &stmt->as.if_.then_block, stmt->as.if_.cond->parenthesized)) {
        return NULL;
    }

    /* Newlines consumed here would only have ended the statement anyway. */
    skip_newlines(p);
    if (!accept(p, TOK_KW_ELSE)) return stmt;
    stmt->as.if_.has_else = true;
    if (!parse_body(p, &stmt->as.if_.else_block, true)) return NULL;
    return stmt;
}

/* Parses one clause of a for loop's header: a statement of a kind that makes
   sense there. is_init tells whether a declaration is allowed. */
static Stmt *parse_for_clause(Parser *p, bool is_init) {
    SrcLoc start = p->tok.loc;
    bool unused;
    Stmt *stmt = parse_statement(p, false, &unused);
    if (!stmt) return NULL;
    bool allowed = stmt->kind == STMT_ASSIGN || stmt->kind == STMT_EXPR ||
                   (is_init && stmt->kind == STMT_VAR);
    if (!allowed) {
        diag_add(p->diags, SEV_ERROR, start,
                 is_init ? "a for loop starts with a declaration, an assignment or a call"
                         : "a for loop's last clause must be an assignment or a call");
        return NULL;
    }
    return stmt;
}

/* Skips what is left of a for loop whose header failed to parse, body
   included, so that its clauses are not then read as statements. */
static Stmt *abandon_for(Parser *p) {
    while (!at(p, TOK_LBRACE) && !at(p, TOK_NEWLINE) && !at(p, TOK_EOF)) next(p);
    int depth = 0;
    while (at(p, TOK_LBRACE) || depth > 0) {
        if (at(p, TOK_EOF)) break;
        if (at(p, TOK_LBRACE)) depth++;
        if (at(p, TOK_RBRACE)) depth--;
        next(p);
    }
    return NULL;
}

static Stmt *parse_for(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_FOR, p->tok.loc);
    next(p);
    bool parenthesized = accept(p, TOK_LPAREN);
    if (parenthesized) skip_newlines(p);

    if (!at(p, TOK_SEMI)) {
        stmt->as.loop.init = parse_for_clause(p, true);
        if (!stmt->as.loop.init) return abandon_for(p);
    }
    if (!expect(p, TOK_SEMI, NULL)) return abandon_for(p);
    if (parenthesized) skip_newlines(p);

    if (!at(p, TOK_SEMI)) {
        stmt->as.loop.cond = parenthesized ? parse_enclosed_expr(p) : parse_expr(p);
        if (!stmt->as.loop.cond) return abandon_for(p);
    }
    if (!expect(p, TOK_SEMI, NULL)) return abandon_for(p);
    if (parenthesized) skip_newlines(p);

    if (!at(p, parenthesized ? TOK_RPAREN : TOK_LBRACE)) {
        stmt->as.loop.step = parse_for_clause(p, false);
        if (!stmt->as.loop.step) return abandon_for(p);
    }
    if (parenthesized) {
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return abandon_for(p);
    }
    if (!parse_body(p, &stmt->as.loop.body, parenthesized)) return NULL;
    return stmt;
}

static Stmt *parse_while(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_WHILE, p->tok.loc);
    next(p);
    stmt->as.loop.cond = parse_expr(p);
    if (!stmt->as.loop.cond) return NULL;
    if (!parse_body(p, &stmt->as.loop.body, stmt->as.loop.cond->parenthesized)) {
        return NULL;
    }
    return stmt;
}

static Stmt *parse_do_while(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_DO_WHILE, p->tok.loc);
    next(p);
    if (!parse_body(p, &stmt->as.loop.body, true)) return NULL;
    skip_newlines(p);
    if (!expect(p, TOK_KW_WHILE, NULL)) return NULL;
    stmt->as.loop.cond = parse_expr(p);
    return stmt->as.loop.cond ? stmt : NULL;
}

static bool compound_assign_op(TokenKind kind, Op *op) {
    switch (kind) {
    case TOK_PLUS_EQ:    *op = OP_ADD; return true;
    case TOK_MINUS_EQ:   *op = OP_SUB; return true;
    case TOK_STAR_EQ:    *op = OP_MUL; return true;
    case TOK_SLASH_EQ:   *op = OP_DIV; return true;
    case TOK_PERCENT_EQ: *op = OP_MOD; return true;
    case TOK_AMP_EQ:     *op = OP_BIT_AND; return true;
    case TOK_PIPE_EQ:    *op = OP_BIT_OR;  return true;
    case TOK_CARET_EQ:   *op = OP_BIT_XOR; return true;
    case TOK_SHL_EQ:     *op = OP_SHL;     return true;
    case TOK_SHR_EQ:     *op = OP_SHR;     return true;
    default:             return false;
    }
}

/* "x++" and "x--" are "x += 1" and "x -= 1". step is the "++" or "--". */
static Stmt *new_step(Parser *p, Token name, Expr *index, Token step) {
    Stmt *stmt = new_stmt(p, STMT_ASSIGN, name.loc);
    stmt->as.assign.name = name.text;
    stmt->as.assign.index = index;
    stmt->as.assign.compound = true;
    stmt->as.assign.op = step.kind == TOK_INC ? OP_ADD : OP_SUB;
    stmt->as.assign.value = new_expr(p, EXPR_INT_LIT, step.loc);
    stmt->as.assign.value->as.int_lit = 1;
    return stmt;
}

/* *needs_terminator is cleared for statements that end themselves: a label,
   and anything closed by a "}". */
static Stmt *parse_statement(Parser *p, bool top_level, bool *needs_terminator) {
    Token tok = p->tok;
    Stmt *stmt;
    Expr *index;
    Op op;
    if (at_type(p)) {
        TypeKind type;
        Token name;
        if (!parse_type(p, &type)) return NULL;
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        if (at(p, TOK_LPAREN)) {
            *needs_terminator = false;
            return parse_func(p, type, name, top_level);
        }
        return parse_decl_rest(p, STMT_VAR, type, name);
    }
    switch (tok.kind) {
    case TOK_KW_CONST: {
        TypeKind type;
        Token name;
        next(p);
        if (!parse_type(p, &type)) return NULL;
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        return parse_decl_rest(p, STMT_CONST, type, name);
    }
    case TOK_INC:
    case TOK_DEC: {
        Token name;
        Expr *index;
        next(p);
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        if (!parse_place_index(p, &index)) return NULL;
        return new_step(p, name, index, tok);
    }
    case TOK_KW_IMPORT: {
        Token path;
        next(p);
        if (!expect(p, TOK_STRING, &path)) return NULL;
        stmt = new_stmt(p, STMT_IMPORT, path.loc);
        stmt->as.import.path = path.text;
        if (at(p, TOK_IDENT) && str_eq_cstr(p->tok.text, "as")) {
            Token alias;
            next(p);
            if (!at(p, TOK_IDENT)) {
                error_expected(p, "a name");
                /* Consumed so that a keyword here is not then read as the
                   start of a statement. */
                if (!at_terminator(p)) next(p);
                return NULL;
            }
            alias = p->tok;
            next(p);
            stmt->as.import.alias = alias.text;
            stmt->as.import.alias_loc = alias.loc;
        }
        if (!top_level) {
            diag_add(p->diags, SEV_ERROR, tok.loc,
                     "an import can only be at the top level of a file");
            return NULL;
        }
        return stmt;
    }
    case TOK_KW_EXPORT:
        next(p);
        if (!at_type(p) && !at(p, TOK_KW_CONST)) {
            error_expected(p, "a function or a constant after 'export'");
            return NULL;
        }
        stmt = parse_statement(p, top_level, needs_terminator);
        if (!stmt) return NULL;
        if (stmt->kind == STMT_VAR) {
            diag_add(p->diags, SEV_ERROR, stmt->loc,
                     "a variable cannot be exported; export a function that returns it");
        } else if (!top_level) {
            diag_add(p->diags, SEV_ERROR, tok.loc,
                     "'export' can only be used at the top level of a file");
        } else {
            stmt->exported = true;
        }
        return stmt;
    case TOK_KW_COUT:
        return parse_cout(p);
    case TOK_KW_CIN:
        return parse_cin(p);
    case TOK_KW_IF:
        *needs_terminator = false;
        return parse_if(p);
    case TOK_KW_WHILE:
        *needs_terminator = false;
        return parse_while(p);
    case TOK_KW_FOR:
        *needs_terminator = false;
        return parse_for(p);
    case TOK_KW_DO:
        return parse_do_while(p);
    case TOK_KW_BREAK:
    case TOK_KW_CONTINUE:
        next(p);
        return new_stmt(p, tok.kind == TOK_KW_BREAK ? STMT_BREAK : STMT_CONTINUE, tok.loc);
    case TOK_KW_GOTO: {
        Token name;
        next(p);
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        stmt = new_stmt(p, STMT_GOTO, name.loc);
        stmt->as.label.name = name.text;
        return stmt;
    }
    case TOK_KW_RETURN:
        next(p);
        stmt = new_stmt(p, STMT_RETURN, tok.loc);
        if (!at_terminator(p)) {
            stmt->as.ret.value = parse_expr(p);
            if (!stmt->as.ret.value) return NULL;
        }
        return stmt;
    case TOK_IDENT:
        next(p);
        if (accept(p, TOK_COLON)) {
            *needs_terminator = false;
            stmt = new_stmt(p, STMT_LABEL, tok.loc);
            stmt->as.label.name = tok.text;
            return stmt;
        }
        if (at(p, TOK_LPAREN)) {
            stmt = new_stmt(p, STMT_EXPR, tok.loc);
            stmt->as.expr = parse_call(p, tok);
            return stmt->as.expr ? stmt : NULL;
        }
        if (at(p, TOK_DOT)) {
            /* What a module exports cannot be assigned, which leaves a call
               as the only statement that starts this way. */
            stmt = new_stmt(p, STMT_EXPR, tok.loc);
            stmt->as.expr = parse_member(p, tok);
            if (!stmt->as.expr) return NULL;
            if (stmt->as.expr->kind != EXPR_CALL) {
                diag_add(p->diags, SEV_ERROR, stmt->as.expr->loc,
                         "what a module exports can be called or read, but not changed");
                return NULL;
            }
            return stmt;
        }
        if (!parse_place_index(p, &index)) return NULL;
        if (accept(p, TOK_ASSIGN)) {
            skip_newlines(p);
            stmt = new_stmt(p, STMT_ASSIGN, tok.loc);
            stmt->as.assign.name = tok.text;
            stmt->as.assign.index = index;
            stmt->as.assign.value = parse_expr(p);
            return stmt->as.assign.value ? stmt : NULL;
        }
        if (at(p, TOK_INC) || at(p, TOK_DEC)) {
            Token step = p->tok;
            next(p);
            return new_step(p, tok, index, step);
        }
        if (compound_assign_op(p->tok.kind, &op)) {
            next(p);
            skip_newlines(p);
            stmt = new_stmt(p, STMT_ASSIGN, tok.loc);
            stmt->as.assign.name = tok.text;
            stmt->as.assign.index = index;
            stmt->as.assign.compound = true;
            stmt->as.assign.op = op;
            stmt->as.assign.value = parse_expr(p);
            return stmt->as.assign.value ? stmt : NULL;
        }
        error_expected(p, index ? "an assignment after the indexed name"
                                : "an assignment, '(', '[' or ':' after a name");
        return NULL;
    default:
        error_expected(p, "a statement");
        /* Consumed so that resynchronizing always makes progress. */
        next(p);
        return NULL;
    }
}

/* Skips the rest of a broken statement, including any braced blocks that
   belong to it. Stopping at a statement keyword keeps a following statement
   intact when the broken one swallowed its newline. */
static void synchronize(Parser *p) {
    int depth = 0;
    for (;;) {
        if (at(p, TOK_EOF)) return;
        if (at(p, TOK_LBRACE)) {
            depth++;
        } else if (at(p, TOK_RBRACE)) {
            if (depth == 0) return;
            depth--;
        } else if (depth == 0 && (at(p, TOK_NEWLINE) || at(p, TOK_SEMI) ||
                                  at_statement_keyword(p))) {
            return;
        }
        next(p);
    }
}

/* Parses statements up to the "}" that closes the enclosing block, or to the
   end of the file at the top level. */
static void parse_statements(Parser *p, Block *block, bool top_level) {
    size_t cap = 0;
    for (;;) {
        while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI)) next(p);
        if (at(p, TOK_EOF)) return;
        if (at(p, TOK_RBRACE)) {
            if (!top_level) return;
            error_expected(p, "a statement");
            next(p);
            continue;
        }
        bool needs_terminator = true;
        SrcLoc start = p->tok.loc;
        Stmt *stmt = parse_statement(p, top_level, &needs_terminator);
        if (!stmt) {
            synchronize(p);
            continue;
        }
        stmt->start = start;
        stmt->end = p->prev;
        if (needs_terminator && !at_terminator(p)) {
            error_expected(p, "end of statement");
            synchronize(p);
        }
        ARENA_PUSH(p->arena, block->stmts, block->count, cap, stmt);
    }
}

static bool parse_block(Parser *p, Block *block) {
    Token open;
    skip_newlines(p);
    if (!expect(p, TOK_LBRACE, &open)) return false;
    block->open = open.loc;
    parse_statements(p, block, false);
    block->close = p->tok.loc;
    if (!at(p, TOK_RBRACE)) {
        diag_add(p->diags, SEV_ERROR, open.loc, "this '{' is never closed");
        return false;
    }
    next(p);
    return true;
}

Program parse_program(Str src, Arena *arena, Diags *diags) {
    Parser p = {.arena = arena, .diags = diags};
    lexer_init(&p.lexer, src, arena, diags);
    next(&p);

    Program program;
    memset(&program, 0, sizeof program);
    parse_statements(&p, &program.top, true);
    program.top.close = p.tok.loc;
    return program;
}
