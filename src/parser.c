#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"

/*
 * Grammar:
 *
 *   program    = { statement } EOF
 *   block      = "{" { statement } "}"
 *   statement  = directive ( statement | block ) | setting terminator
 *              | ( const_decl | var_decl | assign | call | cout | cin | goto
 *                | return | do_while | "break" | "continue" | import
 *                | "export" const_decl | [ "export" ] type_decl
 *                | [ "export" ] extern ) terminator
 *              | if | while | for | [ "export" ] func | label
 *   directive  = "#no_bounds_check" | "#bounds_check"
 *   setting    = "#target" IDENT | "#module" IDENT
 *              | "#board" ( STRING | name up to a space )
 *   import     = "import" ( STRING | path ) [ "as" IDENT ]
 *   path       = the characters up to the next space, as in a/b or ::x/y
 *   extern     = "extern" [ STRING ] [ "link" STRING ]
 *                ( external | "{" { external separator } "}" )
 *   external   = "const" type IDENT | type IDENT "(" [ params ] ")"
 *   type_decl  = "struct" IDENT "{" { type IDENT separator } "}"
 *              | "enum" IDENT "{" { IDENT [ "=" expr ] separator } "}"
 *              | "typedef" type IDENT
 *              | "typedef" ( "struct" | "enum" ) [ IDENT ] "{" ... "}" IDENT
 *   separator  = "," | ";" | NEWLINE
 *   terminator = NEWLINE | ";" | EOF | before "}"
 *
 *   const_decl = "const" type IDENT "=" expr
 *   var_decl   = type IDENT [ "=" expr ]
 *              | type IDENT "[" [ expr ] "]" [ "=" "{" [ expr { "," expr } ] "}" ]
 *   assign     = unary ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|="
 *                      | "^=" | "<<=" | ">>=" ) expr
 *              | unary ( "++" | "--" ) | ( "++" | "--" ) unary
 *   cout       = "cout" "<<" expr { "<<" expr }
 *   cin        = "cin" ">>" target { ">>" target }
 *   target     = type IDENT | IDENT [ "[" expr "]" ] { "." IDENT }
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
 *   func       = type IDENT "(" [ params ] ")" block
 *   params     = "void" | param { "," param }
 *   param      = type IDENT [ "[" "]" ]
 *   type       = base { "*" }
 *   base       = TYPE | a C spelling such as "unsigned long" (see parse_type)
 *              | [ "struct" | "enum" ] name | "typeof" "(" ( type | expr ) ")"
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
 *   unary      = ( "-" | "!" | "~" | "&" | "*" ) unary | postfix
 *   postfix    = primary { "[" expr "]" | "." IDENT | "->" IDENT }
 *   primary    = INT | FLOAT | CHAR | STRING | "true" | "false" | "NULL" | name
 *              | call | type "(" expr ")" | "(" type ")" unary | "(" expr ")"
 *              | "sizeof" "(" type ")" | "sizeof" unary | "cin" | init
 *   init       = "{" [ item { "," item } [ "," ] ] "}"
 *   item       = [ "." IDENT "=" ] expr
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
 * "as" is only special after an import's path, and is otherwise a name. A
 * path written without quotes is whatever the tokens up to the first space
 * spell, so it may hold "/", "." and "::".
 * "IDENT.IDENT" names something that an imported file exports, or a field;
 * which one is decided when the names are resolved.
 *
 * A declared type's name is an ordinary identifier, so a statement that
 * starts with two names in a row ("Point p"), or with a name and a "*"
 * ("Point *p"), is read as a declaration. In "(Name) value" the
 * parenthesized name is a cast when a value follows it, except at the very
 * start of a condition, where "if (ready) run()" must stay a condition and
 * its body; "(Name *)" can only be a cast.
 *
 * A body without braces can start with "*", as in "if (ready) *p = 1":
 * the "*" after a parenthesized condition starts the body when an
 * assignment follows it on the line, and multiplies otherwise.
 *
 * Whether the target of an assignment is something that can be assigned is
 * decided when the program is analyzed. "*p++" is rejected, since C reads it
 * as moving p and it reads here as changing what p points to.
 *
 * The declarations in an extern block share its header, and the "export"
 * written before the block; the block itself leaves no trace in the program,
 * which holds its declarations in its place.
 *
 * A setting says what the file is compiled for, and belongs above every
 * statement of the file that is not one; see file_settings. A board's name
 * holds colons, as in arduino:avr:uno, so it is read the way an import's
 * path is: as the tokens up to the first space, or in quotes.
 *
 * A directive covers the statement, block or function after it, which may
 * start on the next line. A block can only stand on its own after one.
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
    /* Where the condition being parsed starts, so that its opening "(" is
       not taken for a cast; NO_OFFSET outside of one. */
    size_t cond_start;
    /* Where a "*" sits that starts the body of the if or while being
       parsed, and so is not a multiplication; NO_OFFSET if there is none. */
    size_t body_star;
    /* A statement of the file that is not a setting has been read. */
    bool past_settings;
    Arena *arena;
    Diags *diags;
} Parser;

#define NO_OFFSET ((size_t)-1)

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

static bool is_type_word(TokenKind kind) {
    return kind == TOK_TYPE || kind == TOK_KW_UNSIGNED || kind == TOK_KW_SIGNED ||
           kind == TOK_KW_LONG || kind == TOK_KW_SHORT;
}

/* True at a token that can only start a type. A declared type's name is not
   one: it reads as any other identifier. */
static bool at_type(const Parser *p) {
    return is_type_word(p->tok.kind) || at(p, TOK_KW_TYPEOF) || at(p, TOK_KW_STRUCT) ||
           at(p, TOK_KW_ENUM);
}

/* The kind of the token "ahead" places after the current one, newlines
   included. Looks ahead without consuming anything. */
static TokenKind peek_kind(Parser *p, int ahead) {
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    Token tok = p->tok;
    for (int i = 0; i < ahead && tok.kind != TOK_EOF; i++) tok = lexer_next(&lexer);
    /* Problems found while looking ahead are dropped here; they are
       reported when the tokens are parsed. */
    p->diags->count = count;
    p->diags->error_count = error_count;
    return tok.kind;
}

static Expr *parse_enclosed_expr(Parser *p);

/* Called at a name used as a type: "Name" or "module.Name". */
static bool parse_named_type(Parser *p, TypeRef **ref) {
    Token first = p->tok;
    next(p);
    TypeRef *named = arena_alloc(p->arena, sizeof *named);
    memset(named, 0, sizeof *named);
    named->loc = first.loc;
    named->name = first.text;
    named->name_loc = first.loc;
    if (accept(p, TOK_DOT)) {
        Token name;
        if (!expect(p, TOK_IDENT, &name)) return false;
        named->module = first.text;
        named->module_loc = first.loc;
        named->name = name.text;
        named->name_loc = name.loc;
        named->loc = span(first.loc, name.loc);
    }
    *ref = named;
    return true;
}

/*
 * Parses a type without the "*"s that may follow it. Besides Wind's own
 * names this accepts C's multi-word spellings and maps them onto Wind types:
 *
 *   short, short int                   i16     unsigned short       u16
 *   long, long int, long long          i64     unsigned long (long) u64
 *   signed, signed int                 int     unsigned (int)       uint
 *   signed char                        i8      unsigned char        u8
 *   long double                        f128
 *
 * A type that is not built in is returned in *ref, to be resolved when the
 * program is analyzed, with *out left invalid; otherwise *ref is NULL.
 */
static bool parse_type(Parser *p, TypeKind *out, TypeRef **ref);
static bool at_named_pointer_type(Parser *p);

static bool parse_base_type(Parser *p, TypeKind *out, TypeRef **ref) {
    *out = TYPE_INVALID;
    *ref = NULL;
    if (at(p, TOK_KW_STRUCT) || at(p, TOK_KW_ENUM)) {
        /* C's "struct Name"; the keyword adds nothing here. */
        next(p);
        if (!at(p, TOK_IDENT)) {
            error_expected(p, "the name of a type");
            return false;
        }
        return parse_named_type(p, ref);
    }
    if (at(p, TOK_IDENT)) return parse_named_type(p, ref);
    if (at(p, TOK_KW_TYPEOF)) {
        Token keyword = p->tok;
        next(p);
        if (!expect(p, TOK_LPAREN, NULL)) return false;
        skip_newlines(p);
        if (at_type(p) || at_named_pointer_type(p)) {
            if (!parse_type(p, out, ref)) return false;
        } else {
            Expr *of = parse_enclosed_expr(p);
            if (!of) return false;
            TypeRef *computed = arena_alloc(p->arena, sizeof *computed);
            memset(computed, 0, sizeof *computed);
            computed->of = of;
            *ref = computed;
        }
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return false;
        if (*ref && (*ref)->of) (*ref)->loc = span(keyword.loc, p->prev);
        return true;
    }
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
        if (!type_available(*out)) {
            diag_add(p->diags, SEV_ERROR, type.loc,
                     "type '%s' is not available when building for %s", type_name(*out),
                     types_platform()->name);
            return false;
        }
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

/* Parses a type: a base type, and a "*" for each level of pointer to it. A
   pointer to a built-in type is returned in *out like any built-in type. */
static bool parse_type(Parser *p, TypeKind *out, TypeRef **ref) {
    if (!parse_base_type(p, out, ref)) return false;
    while (at(p, TOK_STAR)) {
        if (*ref) {
            (*ref)->depth++;
        } else if (*out == TYPE_VOID) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "a pointer to 'void' is not supported; point to a type");
            next(p);
            return false;
        } else {
            *out = type_pointer_to(*out);
        }
        next(p);
    }
    return true;
}

/* True at a declared type's name, "Name" or "module.Name", that is followed
   by at least one "*" and then ")": a pointer type in parentheses, which no
   expression can look like. Looks ahead without consuming anything. */
static bool at_named_pointer_type(Parser *p) {
    if (!at(p, TOK_IDENT)) return false;
    int ahead = 1;
    TokenKind kind = peek_kind(p, ahead);
    if (kind == TOK_DOT) {
        if (peek_kind(p, ++ahead) != TOK_IDENT) return false;
        kind = peek_kind(p, ++ahead);
    }
    if (kind != TOK_STAR) return false;
    while (kind == TOK_STAR) kind = peek_kind(p, ++ahead);
    return kind == TOK_RPAREN;
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
        if (at_type(p) || at_named_pointer_type(p)) {
            if (!parse_type(p, &expr->as.size_of.type, &expr->as.size_of.type_ref)) {
                return NULL;
            }
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

/* True for a token that, at the start of a line, carries on the expression
   of the line before: one that cannot start a statement. "*" and "&" can
   (as in "*p = 1"), so they carry on only when followed by a space, as a
   binary operator is written; "after" is the token that follows. */
static bool carries_on(Token tok, Token after) {
    switch (tok.kind) {
    case TOK_OR: case TOK_AND:
    case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE:
    case TOK_PIPE: case TOK_CARET: case TOK_SHL: case TOK_SHR:
    case TOK_PLUS: case TOK_MINUS: case TOK_SLASH: case TOK_PERCENT:
    case TOK_QUESTION: case TOK_COLON: case TOK_DOT: case TOK_ARROW:
        return true;
    case TOK_STAR:
    case TOK_AMP:
        return after.loc.offset > tok.loc.offset + tok.loc.len;
    default:
        return false;
    }
}

/* At the end of a line that the next one carries on, moves to the start of
   that line and returns true. Looks ahead without consuming anything
   otherwise. */
static bool carry_on(Parser *p) {
    if (!at(p, TOK_NEWLINE)) return false;
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    Token tok;
    do tok = lexer_next(&lexer);
    while (tok.kind == TOK_NEWLINE);
    Token after = lexer_next(&lexer);
    /* Problems found while looking ahead are dropped here; they are
       reported when the tokens are parsed. */
    p->diags->count = count;
    p->diags->error_count = error_count;
    if (!carries_on(tok, after)) return false;
    skip_newlines(p);
    return true;
}

/* True at a "(" that opens a C-style cast: one holding a type and nothing
   else. Looks ahead without consuming anything. */
static bool at_c_cast(Parser *p) {
    int ahead = 1;
    TokenKind kind = peek_kind(p, ahead);
    if (is_type_word(kind)) {
        while (is_type_word(kind)) kind = peek_kind(p, ++ahead);
        while (kind == TOK_STAR) kind = peek_kind(p, ++ahead);
        return kind == TOK_RPAREN;
    }
    /* A declared type: "(Name)", "(module.Name)" or "(struct Name)". With
       the keyword or a "*" it can be nothing else; without them, the
       parentheses could also hold a variable, so a value has to follow. */
    bool keyword = kind == TOK_KW_STRUCT || kind == TOK_KW_ENUM;
    if (keyword) kind = peek_kind(p, ++ahead);
    if (kind != TOK_IDENT) return false;
    kind = peek_kind(p, ++ahead);
    if (kind == TOK_DOT) {
        if (peek_kind(p, ++ahead) != TOK_IDENT) return false;
        kind = peek_kind(p, ++ahead);
    }
    bool pointer = kind == TOK_STAR;
    while (kind == TOK_STAR) kind = peek_kind(p, ++ahead);
    if (kind != TOK_RPAREN) return false;
    if (keyword || pointer) return true;
    if (p->tok.loc.offset == p->cond_start) return false;
    switch (peek_kind(p, ahead + 1)) {
    case TOK_IDENT:
    case TOK_INT:
    case TOK_FLOAT:
    case TOK_CHAR:
    case TOK_STRING:
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
    case TOK_KW_NULL:
    case TOK_KW_SIZEOF:
    case TOK_LPAREN:
    case TOK_LBRACE:
    case TOK_NOT:
    case TOK_TILDE:
        return true;
    default:
        return false;
    }
}

/* Called at the "{" of a struct value. */
static Expr *parse_init(Parser *p) {
    Token open = p->tok;
    Expr *expr = new_expr(p, EXPR_INIT, open.loc);
    size_t cap = 0;
    next(p);
    skip_newlines(p);
    while (!at(p, TOK_RBRACE)) {
        InitItem item = {.index = -1};
        if (accept(p, TOK_DOT)) {
            Token field;
            if (!expect(p, TOK_IDENT, &field)) return NULL;
            item.field = field.text;
            item.field_loc = field.loc;
            if (!expect(p, TOK_ASSIGN, NULL)) return NULL;
            skip_newlines(p);
        }
        item.value = parse_enclosed_expr(p);
        if (!item.value) return NULL;
        ARENA_PUSH(p->arena, expr->as.init.items, expr->as.init.count, cap, item);
        skip_newlines(p);
        if (!accept(p, TOK_COMMA)) break;
        skip_newlines(p);
    }
    if (!expect(p, TOK_RBRACE, NULL)) return NULL;
    expr->loc = span(open.loc, p->prev);
    return expr;
}

static Expr *parse_primary(Parser *p) {
    Token tok = p->tok;
    Expr *expr;
    if (at_type(p)) {
        TypeKind to;
        TypeRef *to_ref;
        if (!parse_type(p, &to, &to_ref)) return NULL;
        if (!expect(p, TOK_LPAREN, NULL)) return NULL;
        skip_newlines(p);
        Expr *operand = parse_enclosed_expr(p);
        if (!operand) return NULL;
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return NULL;
        expr = new_expr(p, EXPR_CAST, span(tok.loc, p->prev));
        expr->as.cast.to = to;
        expr->as.cast.to_ref = to_ref;
        expr->as.cast.operand = operand;
        return expr;
    }
    switch (tok.kind) {
    case TOK_KW_SIZEOF:
        return parse_sizeof(p);
    case TOK_DOT:
        /* ".field", which only sort takes; the checker says so elsewhere. */
        if (peek_kind(p, 1) != TOK_IDENT) {
            error_expected(p, "an expression");
            return NULL;
        }
        next(p);
        expr = new_expr(p, EXPR_KEY, span(tok.loc, p->tok.loc));
        expr->as.key.name = p->tok.text;
        next(p);
        return expr;
    case TOK_LBRACE:
        return parse_init(p);
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
    case TOK_KW_NULL:
        next(p);
        return new_expr(p, EXPR_NULL, tok.loc);
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
            TypeRef *to_ref;
            next(p);
            if (!parse_type(p, &to, &to_ref)) return NULL;
            if (!expect(p, TOK_RPAREN, NULL)) return NULL;
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            expr = new_expr(p, EXPR_CAST, span(tok.loc, operand->loc));
            expr->as.cast.to = to;
            expr->as.cast.to_ref = to_ref;
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
    Expr *index = NULL;
    if (!at(p, TOK_COLON)) {
        index = parse_enclosed_expr(p);
        if (!index) return NULL;
        skip_newlines(p);
    }
    if (accept(p, TOK_COLON)) {
        /* "base[from:to]", either end of which may be left out. */
        skip_newlines(p);
        Expr *to = NULL;
        if (!at(p, TOK_RBRACKET)) {
            to = parse_enclosed_expr(p);
            if (!to) return NULL;
            skip_newlines(p);
        }
        if (!expect(p, TOK_RBRACKET, NULL)) return NULL;
        Expr *slice = new_expr(p, EXPR_SLICE, span(base->loc, p->prev));
        slice->as.slice.base = base;
        slice->as.slice.from = index;
        slice->as.slice.to = to;
        return slice;
    }
    if (!expect(p, TOK_RBRACKET, NULL)) return NULL;
    Expr *expr = new_expr(p, EXPR_INDEX, span(base->loc, p->prev));
    expr->as.index.base = base;
    expr->as.index.index = index;
    return expr;
}

static Expr *parse_postfix(Parser *p) {
    Expr *expr = parse_primary(p);
    while (expr) {
        carry_on(p);
        if (!at(p, TOK_LBRACKET) && !at(p, TOK_DOT) && !at(p, TOK_ARROW)) break;
        if (at(p, TOK_LBRACKET)) {
            expr = parse_index(p, expr);
            continue;
        }
        Token name;
        bool arrow = at(p, TOK_ARROW);
        next(p);
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        Expr *field = new_expr(p, EXPR_FIELD, span(expr->loc, name.loc));
        field->as.field.base = expr;
        field->as.field.name = name.text;
        field->as.field.name_loc = name.loc;
        field->as.field.arrow = arrow;
        field->as.field.index = -1;
        expr = field;
    }
    return expr;
}

/* Reports a "++" or "--" met inside an expression. */
static Expr *step_in_expression(Parser *p) {
    diag_add(p->diags, SEV_ERROR, p->tok.loc,
             "'%s' is a statement in Wind, not part of an expression; write it on its "
             "own line, before or after the line that uses the value",
             p->tok.kind == TOK_INC ? "++" : "--");
    next(p);
    return NULL;
}

static Expr *parse_unary(Parser *p) {
    Op op;
    switch (p->tok.kind) {
    case TOK_INC:
    case TOK_DEC:   return step_in_expression(p);
    case TOK_MINUS: op = OP_NEG;     break;
    case TOK_NOT:   op = OP_NOT;     break;
    case TOK_TILDE: op = OP_BIT_NOT; break;
    case TOK_AMP:   op = OP_ADDR;    break;
    case TOK_STAR:  op = OP_DEREF;   break;
    default:        return parse_postfix(p);
    }
    Token tok = p->tok;
    next(p);
    Expr *operand = parse_unary(p);
    if (!operand) return NULL;
    Expr *expr = new_expr(p, EXPR_UNARY, span(tok.loc, operand->loc));
    expr->as.unary.op = op;
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
    case TOK_STAR:
        *op = OP_MUL;
        return p->tok.loc.offset == p->body_star ? 0 : 9;
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
        carry_on(p);
        if (at(p, TOK_INC) || at(p, TOK_DEC)) return step_in_expression(p);
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
    case TOK_KW_CERR:
    case TOK_KW_CIN:
    case TOK_KW_IF:
    case TOK_KW_WHILE:
    case TOK_KW_DO:
    case TOK_KW_FOR:
    case TOK_KW_BREAK:
    case TOK_KW_CONTINUE:
    case TOK_KW_GOTO:
    case TOK_KW_DEFER:
    case TOK_KW_RETURN:
    case TOK_KW_IMPORT:
    case TOK_KW_EXPORT:
    case TOK_KW_EXTERN:
    case TOK_DIRECTIVE:
        return true;
    default:
        return false;
    }
}

/* Called at the "[" of an array declaration, after its type and name. */
static Stmt *parse_array_decl(Parser *p, StmtKind kind, TypeKind type, TypeRef *type_ref,
                              Token name) {
    Stmt *stmt = new_stmt(p, kind, name.loc);
    stmt->as.decl.type = type;
    stmt->as.decl.type_ref = type_ref;
    stmt->as.decl.name = name.text;
    stmt->as.decl.is_array = true;
    size_t cap = 0;

    next(p);
    if (at(p, TOK_DOT) && peek_kind(p, 1) == TOK_DOT) {
        next(p);
        next(p);
        stmt->as.decl.is_growable = true;
    } else if (at_type(p)) {
        if (!parse_type(p, &stmt->as.decl.key_type, &stmt->as.decl.key_ref)) return NULL;
        stmt->as.decl.is_map = true;
    } else if (!at(p, TOK_RBRACKET)) {
        stmt->as.decl.length = parse_expr(p);
        if (!stmt->as.decl.length) return NULL;
    }
    if (!expect(p, TOK_RBRACKET, NULL)) return NULL;

    if (accept(p, TOK_ASSIGN)) {
        skip_newlines(p);
        if (stmt->as.decl.is_growable && !at(p, TOK_LBRACE)) {
            /* "= clone(other)", which the checker holds it to. */
            stmt->as.decl.init = parse_expr(p);
            return stmt->as.decl.init ? stmt : NULL;
        }
        if (!expect(p, TOK_LBRACE, NULL)) return NULL;
        skip_newlines(p);
        /* The entries of a map are "key: value"; the key type of a map
           whose key type is a name is not known yet, so a ":" after the
           first element is what tells. */
        bool pairs = stmt->as.decl.is_map;
        while (!at(p, TOK_RBRACE)) {
            Expr *elem = parse_expr(p);
            if (!elem) return NULL;
            if (stmt->as.decl.elem_count == 0) pairs = at(p, TOK_COLON);
            ARENA_PUSH(p->arena, stmt->as.decl.elems, stmt->as.decl.elem_count, cap, elem);
            /* Every element is written as the first one was, with a value
               or without; the checker says what a map or an array needs. */
            if (pairs) {
                if (!expect(p, TOK_COLON, NULL)) return NULL;
                skip_newlines(p);
                Expr *value = parse_expr(p);
                if (!value) return NULL;
                ARENA_PUSH(p->arena, stmt->as.decl.elems, stmt->as.decl.elem_count, cap, value);
            } else if (at(p, TOK_COLON)) {
                diag_add(p->diags, SEV_ERROR, p->tok.loc,
                         "the first element has no ': value', so neither can the others");
                return NULL;
            }
            skip_newlines(p);
            if (!accept(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
        if (!expect(p, TOK_RBRACE, NULL)) return NULL;
        stmt->as.decl.pairs = pairs;
    } else if (kind == STMT_CONST ||
               (!stmt->as.decl.length && !stmt->as.decl.is_growable && !stmt->as.decl.is_map)) {
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

static void synchronize(Parser *p);

/* Called after the type and name of a declaration. */
static Stmt *parse_decl_rest(Parser *p, StmtKind kind, TypeKind type, TypeRef *type_ref,
                             Token name) {
    if (at(p, TOK_LBRACKET)) return parse_array_decl(p, kind, type, type_ref, name);
    Expr *init = NULL;
    if (kind == STMT_CONST || at(p, TOK_ASSIGN)) {
        if (!expect(p, TOK_ASSIGN, NULL)) return NULL;
        skip_newlines(p);
        init = parse_expr(p);
        if (!init) {
            /* The name is declared all the same, so that the lines that use
               it are not reported as well. */
            synchronize(p);
            Stmt *stmt = new_stmt(p, kind, name.loc);
            stmt->as.decl.type = type;
            stmt->as.decl.type_ref = type_ref;
            stmt->as.decl.name = name.text;
            stmt->as.decl.broken = true;
            return stmt;
        }
    }
    if (type == TYPE_VOID) {
        diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                 (int)name.text.len, name.text.data);
        return NULL;
    }
    Stmt *stmt = new_stmt(p, kind, name.loc);
    stmt->as.decl.type = type;
    stmt->as.decl.type_ref = type_ref;
    stmt->as.decl.name = name.text;
    stmt->as.decl.init = init;
    return stmt;
}

/* Called at the "(" of a function's parameters; parses them up to the ")". */
static bool parse_params(Parser *p, Stmt *stmt) {
    size_t cap = 0;
    next(p);
    skip_newlines(p);
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            TypeKind type;
            TypeRef *type_ref;
            Token param_name;
            if (!parse_type(p, &type, &type_ref)) return false;
            /* C's "(void)" for a function without parameters. */
            if (type == TYPE_VOID && stmt->as.func.param_count == 0 &&
                at(p, TOK_RPAREN)) {
                break;
            }
            if (!expect(p, TOK_IDENT, &param_name)) return false;
            if (type == TYPE_VOID) {
                diag_add(p->diags, SEV_ERROR, param_name.loc,
                         "'%.*s' cannot have type 'void'", (int)param_name.text.len,
                         param_name.text.data);
                type = TYPE_INVALID;
            }
            Param param = {.type = type, .type_ref = type_ref, .name = param_name.text,
                           .loc = param_name.loc};
            if (accept(p, TOK_LBRACKET)) {
                if (at(p, TOK_DOT) && peek_kind(p, 1) == TOK_DOT) {
                    next(p);
                    next(p);
                    param.is_growable = true;
                } else if (at_type(p) || at(p, TOK_IDENT)) {
                    if (!parse_type(p, &param.key_type, &param.key_ref)) return false;
                    param.is_map = true;
                } else {
                    param.is_slice = true;
                }
                if (!at(p, TOK_RBRACKET)) {
                    diag_add(p->diags, SEV_ERROR, p->tok.loc,
                             "an array parameter is written '%s %.*s[]', without a "
                             "length, or '%s %.*s[..]' for a growable one",
                             type_name(type), (int)param_name.text.len,
                             param_name.text.data, type_name(type),
                             (int)param_name.text.len, param_name.text.data);
                    return false;
                }
                next(p);
            }
            ARENA_PUSH(p->arena, stmt->as.func.params, stmt->as.func.param_count, cap,
                       param);
            skip_newlines(p);
            if (!accept(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
    }
    return expect(p, TOK_RPAREN, NULL);
}

/* Called at the "(" that follows the function's return type and name. */
static Stmt *parse_func(Parser *p, TypeKind ret, TypeRef *ret_ref, Token name,
                        bool top_level) {
    Stmt *stmt = new_stmt(p, STMT_FUNC, name.loc);
    stmt->as.func.ret = ret;
    stmt->as.func.ret_ref = ret_ref;
    stmt->as.func.name = name.text;
    if (!parse_params(p, stmt)) return NULL;
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
    stmt->as.cout.to_error = at(p, TOK_KW_CERR);
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
    } while (ok && (carry_on(p), at(p, TOK_SHL)));
    p->no_shift = false;
    return ok ? stmt : NULL;
}

/* What follows the name in the target of an assignment or a read. */
typedef struct {
    Expr *index; /* NULL when there is none */
    PlaceField *fields;
    size_t field_count;
} PlaceRest;

/* Parses the optional "[index]" and ".field" steps of a target. Returns
   false on error. */
static bool parse_place_rest(Parser *p, PlaceRest *rest) {
    memset(rest, 0, sizeof *rest);
    size_t cap = 0;
    if (accept(p, TOK_LBRACKET)) {
        skip_newlines(p);
        rest->index = parse_enclosed_expr(p);
        if (!rest->index) return false;
        skip_newlines(p);
        if (!expect(p, TOK_RBRACKET, NULL)) return false;
    }
    while (accept(p, TOK_DOT)) {
        Token name;
        if (!expect(p, TOK_IDENT, &name)) return false;
        PlaceField field = {name.text, name.loc, -1};
        ARENA_PUSH(p->arena, rest->fields, rest->field_count, cap, field);
    }
    return true;
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
        if (at_type(p) || (at(p, TOK_IDENT) && peek_kind(p, 1) == TOK_IDENT)) {
            target.declares = true;
            if (!parse_type(p, &target.type, &target.type_ref)) return NULL;
        }
        if (!expect(p, TOK_IDENT, &name)) return NULL;
        if (target.declares && target.type == TYPE_VOID) {
            diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                     (int)name.text.len, name.text.data);
            return NULL;
        }
        target.name = name.text;
        target.loc = name.loc;
        if (!target.declares) {
            PlaceRest rest;
            if (!parse_place_rest(p, &rest)) return NULL;
            target.index = rest.index;
            target.fields = rest.fields;
            target.field_count = rest.field_count;
        }
        ARENA_PUSH(p->arena, stmt->as.cin.targets, stmt->as.cin.count, cap, target);
    } while (carry_on(p), at(p, TOK_SHR));
    return stmt;
}

static Stmt *parse_statement(Parser *p, bool top_level, bool *needs_terminator);
static bool parse_block(Parser *p, Block *block);
static bool compound_assign_op(TokenKind kind, Op *op);

/* Called at the start of a condition. "if (ready) *p = 1" could read as the
   condition "(ready) * p" with a broken body. If the condition opens with a
   parenthesis whose closing one is followed by "*", and an assignment comes
   before the line or the header ends, that "*" starts the body: returns its
   offset. Returns NO_OFFSET otherwise. Looks ahead without consuming
   anything. */
static size_t body_star(Parser *p) {
    if (!at(p, TOK_LPAREN)) return NO_OFFSET;
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    size_t found = NO_OFFSET;
    int depth = 1;
    Token tok;
    while (depth > 0) {
        tok = lexer_next(&lexer);
        if (tok.kind == TOK_EOF) break;
        if (tok.kind == TOK_LPAREN) depth++;
        if (tok.kind == TOK_RPAREN) depth--;
    }
    if (depth == 0) tok = lexer_next(&lexer);
    if (depth == 0 && tok.kind == TOK_STAR) {
        size_t star = tok.loc.offset;
        for (;;) {
            Op op;
            tok = lexer_next(&lexer);
            if (tok.kind == TOK_LPAREN || tok.kind == TOK_LBRACKET) depth++;
            if (tok.kind == TOK_RPAREN || tok.kind == TOK_RBRACKET) depth--;
            if (tok.kind == TOK_EOF || tok.kind == TOK_NEWLINE || tok.kind == TOK_SEMI ||
                tok.kind == TOK_LBRACE || depth < 0) {
                break;
            }
            bool assigns = tok.kind == TOK_ASSIGN || tok.kind == TOK_INC ||
                           tok.kind == TOK_DEC || compound_assign_op(tok.kind, &op);
            if (depth == 0 && assigns) {
                found = star;
                break;
            }
        }
    }
    /* Problems found while looking ahead are dropped here; they are
       reported when the tokens are parsed. */
    p->diags->count = count;
    p->diags->error_count = error_count;
    return found;
}

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
    if (stmt->kind == STMT_CONST || stmt->kind == STMT_VAR || stmt->kind == STMT_LABEL ||
        stmt->kind == STMT_GROUP || stmt->kind == STMT_DEFER) {
        diag_add(p->diags, SEV_ERROR, start, "a %s cannot be a body without braces",
                 stmt->kind == STMT_LABEL   ? "label"
                 : stmt->kind == STMT_DEFER ? "'defer', which would run at once,"
                                            : "declaration");
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

/* Parses the condition of an if or a while. */
static Expr *parse_condition(Parser *p) {
    p->cond_start = p->tok.loc.offset;
    p->body_star = body_star(p);
    Expr *cond = parse_expr(p);
    p->cond_start = NO_OFFSET;
    p->body_star = NO_OFFSET;
    return cond;
}

static Stmt *parse_if(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_IF, p->tok.loc);
    next(p);
    stmt->as.if_.cond = parse_condition(p);
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

/* True if the header of the for loop starting here, after "for" and any
   "(", goes through something with "in" before any ";". Looks ahead
   without consuming anything. */
static bool at_for_in(Parser *p) {
    Lexer lexer = p->lexer;
    size_t count = p->diags->count;
    size_t error_count = p->diags->error_count;
    bool found = false;
    for (Token tok = p->tok;; tok = lexer_next(&lexer)) {
        if (tok.kind == TOK_SEMI || tok.kind == TOK_LBRACE || tok.kind == TOK_NEWLINE ||
            tok.kind == TOK_EOF || tok.kind == TOK_ASSIGN) {
            break;
        }
        if (tok.kind == TOK_IDENT && str_eq_cstr(tok.text, "in")) {
            found = true;
            break;
        }
    }
    p->diags->count = count;
    p->diags->error_count = error_count;
    return found;
}

/* Parses the rest of "for (T x in over)", "for (A a, B b in over)", after
   the "for" and any "(". */
static Stmt *parse_for_in(Parser *p, Stmt *stmt, bool parenthesized) {
    stmt->kind = STMT_FOR_IN;
    for (;;) {
        Param *each = &stmt->as.loop.each[stmt->as.loop.each_count];
        Token name;
        if (!parse_type(p, &each->type, &each->type_ref)) return abandon_for(p);
        if (!expect(p, TOK_IDENT, &name)) return abandon_for(p);
        each->name = name.text;
        each->loc = name.loc;
        stmt->as.loop.each_count++;
        if (stmt->as.loop.each_count == 2 || !accept(p, TOK_COMMA)) break;
        skip_newlines(p);
    }
    if (!at(p, TOK_IDENT) || !str_eq_cstr(p->tok.text, "in")) {
        error_expected(p, stmt->as.loop.each_count == 2
                              ? "'in' after the two loop variables"
                              : "'in', or ',' and a second loop variable");
        return abandon_for(p);
    }
    next(p);
    skip_newlines(p);
    stmt->as.loop.over = parenthesized ? parse_enclosed_expr(p) : parse_expr(p);
    if (!stmt->as.loop.over) return abandon_for(p);
    if (parenthesized) {
        skip_newlines(p);
        if (!expect(p, TOK_RPAREN, NULL)) return abandon_for(p);
    }
    if (!parse_body(p, &stmt->as.loop.body, parenthesized)) return NULL;
    return stmt;
}

static Stmt *parse_for(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_FOR, p->tok.loc);
    next(p);
    bool parenthesized = accept(p, TOK_LPAREN);
    if (parenthesized) skip_newlines(p);
    if (at_for_in(p)) return parse_for_in(p, stmt, parenthesized);

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
    stmt->as.loop.cond = parse_condition(p);
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

/* An assignment's location is the name it starts with, when it starts with
   one, else its whole target. */
static Stmt *new_assign(Parser *p, Token first, Expr *target) {
    Stmt *stmt = new_stmt(p, STMT_ASSIGN, first.kind == TOK_IDENT ? first.loc : target->loc);
    stmt->as.assign.target = target;
    return stmt;
}

/* "x++" and "x--" are "x += 1" and "x -= 1". step is the "++" or "--". */
static Stmt *new_step(Parser *p, Token first, Expr *target, Token step) {
    Stmt *stmt = new_assign(p, first, target);
    stmt->as.assign.compound = true;
    stmt->as.assign.op = step.kind == TOK_INC ? OP_ADD : OP_SUB;
    stmt->as.assign.value = new_expr(p, EXPR_INT_LIT, step.loc);
    stmt->as.assign.value->as.int_lit = 1;
    return stmt;
}

/* Called after the expression that starts a statement: parses the
   assignment to it, or takes it as the statement if it is a call. first is
   the statement's first token. */
static Stmt *parse_assign_rest(Parser *p, Token first, Expr *target) {
    Stmt *stmt;
    Op op;
    if (target->kind == EXPR_CALL) {
        stmt = new_stmt(p, STMT_EXPR, first.loc);
        stmt->as.expr = target;
        return stmt;
    }
    if (accept(p, TOK_ASSIGN)) {
        skip_newlines(p);
        stmt = new_assign(p, first, target);
        stmt->as.assign.value = parse_expr(p);
        return stmt->as.assign.value ? stmt : NULL;
    }
    if (at(p, TOK_INC) || at(p, TOK_DEC)) {
        Token step = p->tok;
        if (target->kind == EXPR_UNARY && target->as.unary.op == OP_DEREF &&
            !target->parenthesized) {
            diag_add(p->diags, SEV_ERROR, step.loc,
                     "'%s' after '*' is unclear: put the '*' in parentheses to change "
                     "what the pointer points to, or leave the '*' out to move the "
                     "pointer",
                     step.kind == TOK_INC ? "++" : "--");
            next(p);
            return NULL;
        }
        next(p);
        return new_step(p, first, target, step);
    }
    if (compound_assign_op(p->tok.kind, &op)) {
        next(p);
        skip_newlines(p);
        stmt = new_assign(p, first, target);
        stmt->as.assign.compound = true;
        stmt->as.assign.op = op;
        stmt->as.assign.value = parse_expr(p);
        return stmt->as.assign.value ? stmt : NULL;
    }
    bool bare_name = target->kind == EXPR_IDENT && !target->as.ident.module.len;
    error_expected(p, bare_name ? "an assignment, '(', '[', '.' or ':' after a name"
                                : "an assignment after the place it names");
    return NULL;
}

/* Parses the fields of a struct, from its "{" to its "}". */
static bool parse_struct_body(Parser *p, Stmt *stmt) {
    Token open;
    size_t cap = 0;
    if (!expect(p, TOK_LBRACE, &open)) return false;
    for (;;) {
        while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI) || at(p, TOK_COMMA)) next(p);
        if (at(p, TOK_RBRACE) || at(p, TOK_EOF)) break;
        Member field = {0};
        Token name;
        if (!parse_type(p, &field.type, &field.type_ref)) return false;
        if (!expect(p, TOK_IDENT, &name)) return false;
        field.name = name.text;
        field.loc = name.loc;
        if (field.type == TYPE_VOID) {
            diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                     (int)name.text.len, name.text.data);
            field.type = TYPE_INVALID;
        }
        if (accept(p, TOK_LBRACKET)) {
            if (at(p, TOK_DOT) && peek_kind(p, 1) == TOK_DOT) {
                next(p);
                next(p);
                field.is_growable = true;
            } else if (at_type(p) || at(p, TOK_IDENT)) {
                if (!parse_type(p, &field.key_type, &field.key_ref)) return false;
                field.is_map = true;
            } else {
                diag_add(p->diags, SEV_ERROR, p->tok.loc,
                         "a field cannot be an array of a fixed length; '[..]' makes it a "
                         "growable one");
                /* Skipped so that the rest of the struct is still read. */
                while (!at(p, TOK_RBRACKET) && !at(p, TOK_NEWLINE) && !at(p, TOK_RBRACE) &&
                       !at(p, TOK_EOF)) {
                    next(p);
                }
            }
            if (!expect(p, TOK_RBRACKET, NULL)) return false;
        }
        if (!at(p, TOK_NEWLINE) && !at(p, TOK_SEMI) && !at(p, TOK_COMMA) &&
            !at(p, TOK_RBRACE)) {
            error_expected(p, "the end of the field");
            return false;
        }
        ARENA_PUSH(p->arena, stmt->as.type.members, stmt->as.type.member_count, cap, field);
    }
    if (!at(p, TOK_RBRACE)) {
        diag_add(p->diags, SEV_ERROR, open.loc, "this '{' is never closed");
        return false;
    }
    next(p);
    return true;
}

/* Parses the names of an enum, from its "{" to its "}". */
static bool parse_enum_body(Parser *p, Stmt *stmt) {
    Token open;
    size_t cap = 0;
    if (!expect(p, TOK_LBRACE, &open)) return false;
    for (;;) {
        while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI) || at(p, TOK_COMMA)) next(p);
        if (at(p, TOK_RBRACE) || at(p, TOK_EOF)) break;
        Member item = {0};
        Token name;
        if (!expect(p, TOK_IDENT, &name)) return false;
        item.name = name.text;
        item.loc = name.loc;
        if (accept(p, TOK_ASSIGN)) {
            skip_newlines(p);
            item.value = parse_enclosed_expr(p);
            if (!item.value) return false;
        }
        if (!at(p, TOK_NEWLINE) && !at(p, TOK_SEMI) && !at(p, TOK_COMMA) &&
            !at(p, TOK_RBRACE)) {
            error_expected(p, "',' or '}'");
            return false;
        }
        ARENA_PUSH(p->arena, stmt->as.type.members, stmt->as.type.member_count, cap, item);
    }
    if (!at(p, TOK_RBRACE)) {
        diag_add(p->diags, SEV_ERROR, open.loc, "this '{' is never closed");
        return false;
    }
    next(p);
    return true;
}

/* True at "struct" or "enum" when it starts a declaration of the type,
   and not the use of one as in "struct Point p". */
static bool at_type_body(Parser *p) {
    if (!at(p, TOK_KW_STRUCT) && !at(p, TOK_KW_ENUM)) return false;
    TokenKind next_kind = peek_kind(p, 1);
    return next_kind == TOK_LBRACE ||
           (next_kind == TOK_IDENT && peek_kind(p, 2) == TOK_LBRACE);
}

/* Called at "struct" or "enum" that starts a declaration, or at "typedef". */
static Stmt *parse_type_decl(Parser *p, bool top_level, bool *needs_terminator) {
    Token keyword = p->tok;
    bool is_typedef = accept(p, TOK_KW_TYPEDEF);
    Stmt *stmt = new_stmt(p, STMT_TYPE, keyword.loc);
    Token name = {0};

    if (at_type_body(p)) {
        bool is_enum = at(p, TOK_KW_ENUM);
        stmt->as.type.kind = is_enum ? TYPE_DECL_ENUM : TYPE_DECL_STRUCT;
        next(p);
        bool has_tag = at(p, TOK_IDENT);
        Token tag = p->tok;
        if (has_tag) next(p);
        if (!(is_enum ? parse_enum_body(p, stmt) : parse_struct_body(p, stmt))) return NULL;
        if (is_typedef) {
            if (!expect(p, TOK_IDENT, &name)) return NULL;
            if (has_tag && !str_eq(tag.text, name.text)) {
                stmt->as.type.tag = tag.text;
                stmt->as.type.tag_loc = tag.loc;
            }
        } else if (has_tag) {
            name = tag;
            *needs_terminator = false;
        } else {
            diag_add(p->diags, SEV_ERROR, keyword.loc, "this %s needs a name",
                     is_enum ? "enum" : "struct");
            return NULL;
        }
    } else {
        stmt->as.type.kind = TYPE_DECL_ALIAS;
        if (!parse_type(p, &stmt->as.type.type, &stmt->as.type.type_ref)) return NULL;
        if (!expect(p, TOK_IDENT, &name)) return NULL;
    }
    stmt->loc = name.loc;
    stmt->as.type.name = name.text;
    if (!top_level) {
        diag_add(p->diags, SEV_ERROR, keyword.loc,
                 "a type can only be declared at the top level of a file");
        return NULL;
    }
    return stmt;
}

/* Called after the type that starts a declaration: parses the name and
   the function or variable that follows. */
static Stmt *parse_typed_decl(Parser *p, StmtKind kind, TypeKind type, TypeRef *type_ref,
                              bool top_level, bool *needs_terminator) {
    Token name;
    if (!expect(p, TOK_IDENT, &name)) return NULL;
    if (kind == STMT_VAR && at(p, TOK_LPAREN)) {
        *needs_terminator = false;
        return parse_func(p, type, type_ref, name, top_level);
    }
    return parse_decl_rest(p, kind, type, type_ref, name);
}

/* Called at a directive: parses it and the statement, block or function it
   covers. */
static bool is_setting(Str name) {
    return str_eq_cstr(name, "target") || str_eq_cstr(name, "board") ||
           str_eq_cstr(name, "module");
}

/* Called at "#target" or "#board": parses the setting and its value. */
static Stmt *parse_setting(Parser *p, bool top_level) {
    Token directive = p->tok;
    /* The settings other than #board take a name. */
    bool is_target = !str_eq_cstr(directive.text, "board");
    next(p);
    Stmt *stmt = new_stmt(p, STMT_SETTING, directive.loc);
    stmt->as.setting.name = directive.text;
    stmt->as.setting.value = p->tok.text;
    stmt->as.setting.value_loc = p->tok.loc;
    if (is_target) {
        if (!expect(p, TOK_IDENT, NULL)) {
            /* Consumed so that a keyword here is not then read as the start
               of a statement. */
            if (!at_terminator(p)) next(p);
            return NULL;
        }
    } else if (at(p, TOK_STRING)) {
        next(p);
    } else if (at_terminator(p)) {
        error_expected(p, "the board's name, as in '#board arduino:avr:uno'");
        return NULL;
    } else {
        SrcLoc first = p->tok.loc;
        size_t end = first.offset;
        while (!at_terminator(p) && !at(p, TOK_STRING) && p->tok.loc.offset == end) {
            end = p->tok.loc.offset + p->tok.loc.len;
            next(p);
        }
        stmt->as.setting.value = (Str){p->lexer.src.data + first.offset, end - first.offset};
        stmt->as.setting.value_loc = span(first, p->prev);
    }
    if (!top_level) {
        diag_add(p->diags, SEV_ERROR, directive.loc,
                 "'#%.*s' goes at the top of the file, not inside a block",
                 (int)directive.text.len, directive.text.data);
        return NULL;
    }
    return stmt;
}

static Stmt *parse_directive(Parser *p, bool top_level, bool *needs_terminator) {
    Token directive = p->tok;
    if (is_setting(directive.text)) return parse_setting(p, top_level);
    BoundsMode bounds = BOUNDS_INHERIT;
    if (str_eq_cstr(directive.text, "no_bounds_check")) bounds = BOUNDS_UNCHECKED;
    if (str_eq_cstr(directive.text, "bounds_check")) bounds = BOUNDS_CHECKED;
    next(p);
    if (bounds == BOUNDS_INHERIT) {
        diag_add(p->diags, SEV_ERROR, directive.loc,
                 "unknown directive '#%.*s'; there are '#no_bounds_check', "
                 "'#bounds_check', '#target', '#board' and '#module'",
                 (int)directive.text.len, directive.text.data);
        return NULL;
    }
    skip_newlines(p);
    Stmt *stmt;
    if (at(p, TOK_LBRACE)) {
        stmt = new_stmt(p, STMT_BLOCK, directive.loc);
        if (!parse_block(p, &stmt->as.block)) return NULL;
        *needs_terminator = false;
    } else {
        stmt = parse_statement(p, top_level, needs_terminator);
        if (!stmt) return NULL;
    }
    bool is_code = stmt->kind != STMT_TYPE && stmt->kind != STMT_IMPORT &&
                   stmt->kind != STMT_LABEL && stmt->kind != STMT_CONST &&
                   stmt->kind != STMT_GROUP && !stmt->external;
    if (!is_code) {
        diag_add(p->diags, SEV_ERROR, directive.loc,
                 "'#%.*s' goes in front of code: a statement, a block or a function",
                 (int)directive.text.len, directive.text.data);
        return stmt;
    }
    /* Of two directives on one statement, the one nearer to it holds. */
    if (stmt->bounds == BOUNDS_INHERIT) stmt->bounds = bounds;
    return stmt;
}

/* Parses one external declaration, after its "extern" and header: a
   constant or a function. */
static Stmt *parse_external(Parser *p, Str header, Str link) {
    if (at(p, TOK_KW_EXPORT) || at(p, TOK_KW_EXTERN)) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc,
                 "'%s' goes before the block's 'extern', where it covers every "
                 "declaration in it",
                 at(p, TOK_KW_EXPORT) ? "export" : "extern");
        next(p);
        return NULL;
    }
    bool is_const = accept(p, TOK_KW_CONST);
    TypeKind type;
    TypeRef *type_ref;
    Token name;
    if (!parse_type(p, &type, &type_ref)) return NULL;
    if (!expect(p, TOK_IDENT, &name)) return NULL;
    Stmt *stmt;
    if (is_const) {
        stmt = new_stmt(p, STMT_CONST, name.loc);
        stmt->as.decl.type = type;
        stmt->as.decl.type_ref = type_ref;
        stmt->as.decl.name = name.text;
        if (at(p, TOK_ASSIGN) || at(p, TOK_LBRACKET)) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "an external constant takes its value from C; write only its type and "
                     "name");
            return NULL;
        }
    } else {
        if (!at(p, TOK_LPAREN)) {
            error_expected(p, "'(' after the name of an external function, or 'const' "
                              "before the type of an external constant");
            return NULL;
        }
        stmt = new_stmt(p, STMT_FUNC, name.loc);
        stmt->as.func.ret = type;
        stmt->as.func.ret_ref = type_ref;
        stmt->as.func.name = name.text;
        if (!parse_params(p, stmt)) return NULL;
        if (at(p, TOK_LBRACE)) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "an external function is defined in C and has no body here");
            return NULL;
        }
    }
    if (type == TYPE_VOID && is_const) {
        diag_add(p->diags, SEV_ERROR, name.loc, "'%.*s' cannot have type 'void'",
                 (int)name.text.len, name.text.data);
        return NULL;
    }
    stmt->external = true;
    stmt->header = header;
    stmt->link = link;
    return stmt;
}

/* Parses the declarations of an extern block, from its "{" to its "}". A
   declaration that fails is reported and skipped, and the rest are still
   read. */
static Stmt *parse_extern_group(Parser *p, SrcLoc keyword, Str header, Str link) {
    Stmt *group = new_stmt(p, STMT_GROUP, keyword);
    Block *block = &group->as.block;
    size_t cap = 0;
    block->open = p->tok.loc;
    next(p);
    for (;;) {
        while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI)) next(p);
        if (at(p, TOK_RBRACE) || at(p, TOK_EOF)) break;
        SrcLoc start = p->tok.loc;
        Stmt *stmt = parse_external(p, header, link);
        if (stmt && !at_terminator(p)) {
            error_expected(p, "the end of the declaration");
            stmt = NULL;
        }
        if (!stmt) {
            /* Skipped up to the end of the line, over any braces on the way,
               so that the "}" of a mistaken body does not end the block. */
            int depth = 0;
            while (!at(p, TOK_EOF) &&
                   (depth > 0 || (!at(p, TOK_NEWLINE) && !at(p, TOK_SEMI) && !at(p, TOK_RBRACE)))) {
                if (at(p, TOK_LBRACE)) depth++;
                if (at(p, TOK_RBRACE)) depth--;
                next(p);
            }
            continue;
        }
        stmt->start = start;
        stmt->end = p->prev;
        ARENA_PUSH(p->arena, block->stmts, block->count, cap, stmt);
    }
    block->close = p->tok.loc;
    if (!at(p, TOK_RBRACE)) {
        diag_add(p->diags, SEV_ERROR, block->open, "this '{' is never closed");
        return NULL;
    }
    next(p);
    return group;
}

/* Called at "extern": parses the external function or constant it
   declares, or the block of them. */
static Stmt *parse_extern(Parser *p, bool top_level, bool *needs_terminator) {
    Token keyword = p->tok;
    Str header = {0};
    next(p);
    if (at(p, TOK_STRING)) {
        header = p->tok.text;
        /* The name goes into an #include line, which ends at a quote or
           a line break. */
        bool usable = header.len > 0;
        for (size_t i = 0; i < header.len; i++) {
            unsigned char c = (unsigned char)header.data[i];
            if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') usable = false;
        }
        if (!usable) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "a header's name cannot be empty or hold quotes, backslashes or "
                     "control characters");
            return NULL;
        }
        next(p);
    }
    /* "link" is only special here, after the header. */
    Str link = {0};
    if (at(p, TOK_IDENT) && str_eq_cstr(p->tok.text, "link")) {
        next(p);
        if (!at(p, TOK_STRING)) {
            error_expected(p, "the name of a C library, as in 'link \"m\"'");
            return NULL;
        }
        link = p->tok.text;
        /* The name becomes "-l" and it on the C compiler's command line. */
        bool usable = link.len > 0 && link.data[0] != '-';
        for (size_t i = 0; i < link.len; i++) {
            char c = link.data[i];
            bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' ||
                           c == '-';
            if (!allowed) usable = false;
        }
        if (!usable) {
            diag_add(p->diags, SEV_ERROR, p->tok.loc,
                     "a library's name is made of letters, digits, '_', '.', '+' and '-', "
                     "and does not start with '-'");
            return NULL;
        }
        next(p);
    }
    Stmt *stmt;
    if (at(p, TOK_LBRACE)) {
        *needs_terminator = false;
        stmt = parse_extern_group(p, keyword.loc, header, link);
    } else {
        stmt = parse_external(p, header, link);
    }
    if (stmt && !top_level) {
        diag_add(p->diags, SEV_ERROR, keyword.loc,
                 "'extern' can only be used at the top level of a file");
        return NULL;
    }
    return stmt;
}

/* Called after "import": parses the path, written in quotes or as the
   tokens up to the next space. */
static bool parse_import_path(Parser *p, Stmt *stmt) {
    if (at(p, TOK_STRING)) {
        stmt->loc = p->tok.loc;
        stmt->as.import.path = p->tok.text;
        stmt->as.import.quoted = true;
        next(p);
        return true;
    }
    if (at_terminator(p) || at(p, TOK_KW_ELSE)) {
        error_expected(p, "the path of a file to import");
        return false;
    }
    SrcLoc first = p->tok.loc;
    size_t end = first.offset;
    while (!at_terminator(p) && !at(p, TOK_STRING) && p->tok.loc.offset == end) {
        end = p->tok.loc.offset + p->tok.loc.len;
        next(p);
    }
    stmt->as.import.path = (Str){p->lexer.src.data + first.offset, end - first.offset};
    stmt->loc = span(first, p->prev);
    return true;
}

/* *needs_terminator is cleared for statements that end themselves: a label,
   and anything closed by a "}". */
/* Marks a declaration, or each declaration of a group, as exported, or
   reports why it cannot be. */
static void export_stmt(Parser *p, Stmt *stmt) {
    if (stmt->kind == STMT_VAR) {
        diag_add(p->diags, SEV_ERROR, stmt->loc,
                 "a variable cannot be exported; make it a constant if its value never "
                 "changes, or export a function that returns it");
    } else if (stmt->kind == STMT_GROUP) {
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            export_stmt(p, stmt->as.block.stmts[i]);
        }
    } else if (stmt->kind != STMT_CONST && stmt->kind != STMT_FUNC &&
               stmt->kind != STMT_TYPE) {
        diag_add(p->diags, SEV_ERROR, stmt->loc,
                 "only a function, a constant or a type can be exported");
    } else {
        stmt->exported = true;
    }
}

/* Parses one declaration of an export block, and exports it. */
static Stmt *parse_exported(Parser *p, bool top_level, bool *needs_terminator) {
    if (at(p, TOK_KW_EXPORT)) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc,
                 "this is already exported by the block's 'export'");
        next(p);
        return NULL;
    }
    Stmt *stmt = parse_statement(p, top_level, needs_terminator);
    if (stmt) export_stmt(p, stmt);
    return stmt;
}

/* Parses one declaration of a const block: a type, a name and a value. */
static Stmt *parse_constant(Parser *p, bool top_level, bool *needs_terminator) {
    if (at(p, TOK_KW_CONST)) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc,
                 "this is already constant by the block's 'const'");
        next(p);
        return NULL;
    }
    if (!at_type(p) && !at(p, TOK_IDENT)) {
        diag_add(p->diags, SEV_ERROR, p->tok.loc,
                 "a 'const' block holds only constants, written as 'int SIZE = 4'");
        next(p);
        return NULL;
    }
    TypeKind type;
    TypeRef *type_ref;
    if (!parse_type(p, &type, &type_ref)) return NULL;
    return parse_typed_decl(p, STMT_CONST, type, type_ref, top_level, needs_terminator);
}

/* Parses a block of declarations that share something written once before
   it, from its "{" to its "}", reading each with parse_item. A declaration
   that fails is reported and skipped, and the rest are still read. */
static Stmt *parse_group(Parser *p, SrcLoc keyword, bool top_level,
                         Stmt *(*parse_item)(Parser *, bool, bool *)) {
    Stmt *group = new_stmt(p, STMT_GROUP, keyword);
    Block *block = &group->as.block;
    size_t cap = 0;
    block->open = p->tok.loc;
    next(p);
    for (;;) {
        while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI)) next(p);
        if (at(p, TOK_RBRACE) || at(p, TOK_EOF)) break;
        bool needs_terminator = true;
        SrcLoc start = p->tok.loc;
        size_t errors_before = p->diags->error_count;
        Stmt *stmt = parse_item(p, top_level, &needs_terminator);
        if (!stmt) {
            synchronize(p);
            continue;
        }
        stmt->start = start;
        stmt->end = p->prev;
        if (needs_terminator && !at_terminator(p)) {
            if (p->diags->error_count == errors_before) error_expected(p, "end of statement");
            synchronize(p);
        }
        Stmt **items = stmt->kind == STMT_GROUP ? stmt->as.block.stmts : &stmt;
        size_t count = stmt->kind == STMT_GROUP ? stmt->as.block.count : 1;
        for (size_t i = 0; i < count; i++) {
            ARENA_PUSH(p->arena, block->stmts, block->count, cap, items[i]);
        }
    }
    if (!at(p, TOK_RBRACE)) {
        diag_add(p->diags, SEV_ERROR, block->open, "this '{' is never closed");
        return group;
    }
    block->close = p->tok.loc;
    next(p);
    return group;
}

static Stmt *parse_statement(Parser *p, bool top_level, bool *needs_terminator) {
    Token tok = p->tok;
    Stmt *stmt;
    Expr *target;
    TypeKind type;
    TypeRef *type_ref;
    if (at(p, TOK_DIRECTIVE)) return parse_directive(p, top_level, needs_terminator);
    if (at_type_body(p)) return parse_type_decl(p, top_level, needs_terminator);
    if (at_type(p)) {
        if (!parse_type(p, &type, &type_ref)) return NULL;
        return parse_typed_decl(p, STMT_VAR, type, type_ref, top_level, needs_terminator);
    }
    switch (tok.kind) {
    case TOK_KW_TYPEDEF:
        return parse_type_decl(p, top_level, needs_terminator);
    case TOK_KW_CONST:
        next(p);
        if (at(p, TOK_LBRACE)) {
            *needs_terminator = false;
            return parse_group(p, tok.loc, top_level, parse_constant);
        }
        if (!parse_type(p, &type, &type_ref)) return NULL;
        return parse_typed_decl(p, STMT_CONST, type, type_ref, top_level, needs_terminator);
    case TOK_INC:
    case TOK_DEC: {
        next(p);
        Token first = p->tok;
        target = parse_unary(p);
        return target ? new_step(p, first, target, tok) : NULL;
    }
    case TOK_STAR:
    case TOK_LPAREN:
        /* An assignment through a pointer: "*p = v", "(*p).x = v". */
        target = parse_unary(p);
        return target ? parse_assign_rest(p, tok, target) : NULL;
    case TOK_KW_EXTERN:
        return parse_extern(p, top_level, needs_terminator);
    case TOK_KW_IMPORT: {
        next(p);
        stmt = new_stmt(p, STMT_IMPORT, tok.loc);
        if (!parse_import_path(p, stmt)) return NULL;
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
        if (!top_level) {
            diag_add(p->diags, SEV_ERROR, tok.loc,
                     "'export' can only be used at the top level of a file");
            return NULL;
        }
        if (at(p, TOK_LBRACE)) {
            *needs_terminator = false;
            return parse_group(p, tok.loc, top_level, parse_exported);
        }
        if (!at_type(p) && !at(p, TOK_KW_CONST) && !at(p, TOK_KW_TYPEDEF) &&
            !at(p, TOK_KW_EXTERN) && !at(p, TOK_IDENT)) {
            error_expected(p, "a function, a constant, a type or '{' after 'export'");
            return NULL;
        }
        stmt = parse_statement(p, top_level, needs_terminator);
        if (stmt) export_stmt(p, stmt);
        return stmt;
    case TOK_KW_COUT:
    case TOK_KW_CERR:
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
    case TOK_KW_DEFER: {
        next(p);
        stmt = new_stmt(p, STMT_DEFER, tok.loc);
        Stmt *deferred;
        if (at(p, TOK_LBRACE)) {
            deferred = new_stmt(p, STMT_BLOCK, p->tok.loc);
            if (!parse_block(p, &deferred->as.block)) return NULL;
            *needs_terminator = false;
        } else {
            SrcLoc start = p->tok.loc;
            deferred = parse_statement(p, false, needs_terminator);
            if (!deferred) return NULL;
            deferred->start = start;
            deferred->end = p->prev;
        }
        if (deferred->kind == STMT_CONST || deferred->kind == STMT_VAR ||
            deferred->kind == STMT_LABEL || deferred->kind == STMT_GROUP) {
            diag_add(p->diags, SEV_ERROR, deferred->loc,
                     "a declaration or a label cannot be deferred; it would be gone "
                     "before it ran");
            return NULL;
        }
        stmt->as.deferred = deferred;
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
    case TOK_IDENT: {
        /* A name followed by another name or by "*", the first of which
           may be qualified by a module, declares something of a declared
           type; no other statement starts that way. */
        TokenKind second = peek_kind(p, 1);
        bool qualified = second == TOK_DOT && peek_kind(p, 2) == TOK_IDENT;
        TokenKind after = qualified ? peek_kind(p, 3) : second;
        if (after == TOK_IDENT || after == TOK_STAR) {
            if (!parse_type(p, &type, &type_ref)) return NULL;
            return parse_typed_decl(p, STMT_VAR, type, type_ref, top_level,
                                    needs_terminator);
        }
        if (second == TOK_COLON) {
            next(p);
            next(p);
            *needs_terminator = false;
            stmt = new_stmt(p, STMT_LABEL, tok.loc);
            stmt->as.label.name = tok.text;
            return stmt;
        }
        target = parse_unary(p);
        return target ? parse_assign_rest(p, tok, target) : NULL;
    }
    default: {
        /* "&" and "*" can start an expression, and the statement is more
           likely to be meant as one. */
        Op op;
        if (binary_precedence(p, &op) && !at(p, TOK_AMP) && !at(p, TOK_STAR)) {
            diag_add(p->diags, SEV_ERROR, tok.loc,
                     "a statement cannot start with %s, and the statement before it "
                     "is complete, so the line cannot carry it on",
                     token_kind_name(tok.kind));
        } else {
            error_expected(p, "a statement");
        }
        /* Consumed so that resynchronizing always makes progress. */
        next(p);
        return NULL;
    }
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
        size_t errors_before = p->diags->error_count;
        Stmt *stmt = parse_statement(p, top_level, &needs_terminator);
        if (!stmt) {
            synchronize(p);
            continue;
        }
        stmt->start = start;
        stmt->end = p->prev;
        if (needs_terminator && !at_terminator(p)) {
            /* A statement that has reported its error has said enough. */
            if (p->diags->error_count == errors_before) error_expected(p, "end of statement");
            synchronize(p);
        }
        if (top_level && stmt->kind == STMT_SETTING) {
            stmt->as.setting.leading = !p->past_settings;
        } else if (top_level) {
            p->past_settings = true;
        }
        if (stmt->kind == STMT_GROUP) {
            for (size_t i = 0; i < stmt->as.block.count; i++) {
                ARENA_PUSH(p->arena, block->stmts, block->count, cap, stmt->as.block.stmts[i]);
            }
            continue;
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
    Parser p = {.arena = arena, .diags = diags, .cond_start = NO_OFFSET,
                .body_star = NO_OFFSET};
    lexer_init(&p.lexer, src, arena, diags);
    next(&p);

    Program program;
    memset(&program, 0, sizeof program);
    parse_statements(&p, &program.top, true);
    program.top.close = p.tok.loc;
    return program;
}

static char *copy_text(Str text) {
    char *copy = xmalloc(text.len + 1);
    memcpy(copy, text.data, text.len);
    copy[text.len] = '\0';
    return copy;
}

FileSettings file_settings(Str src) {
    FileSettings settings = {0};
    Arena arena = {0};
    Diags diags = {.arena = &arena};
    Lexer lexer;
    lexer_init(&lexer, src, &arena, &diags);
    Token tok = lexer_next(&lexer);
    for (;;) {
        while (tok.kind == TOK_NEWLINE || tok.kind == TOK_SEMI) tok = lexer_next(&lexer);
        if (tok.kind != TOK_DIRECTIVE || !is_setting(tok.text)) break;
        if (str_eq_cstr(tok.text, "module")) {
            /* Not about what the file is compiled for: skipped. */
            tok = lexer_next(&lexer);
            if (tok.kind != TOK_IDENT) break;
            tok = lexer_next(&lexer);
            continue;
        }
        bool is_target = str_eq_cstr(tok.text, "target");
        Token value = lexer_next(&lexer);
        tok = lexer_next(&lexer);
        Str text = value.text;
        if (!is_target && value.kind != TOK_STRING) {
            /* A name without quotes: the tokens up to the first space. */
            size_t end = value.loc.offset + value.loc.len;
            while (tok.kind != TOK_NEWLINE && tok.kind != TOK_SEMI && tok.kind != TOK_EOF &&
                   tok.kind != TOK_STRING && tok.loc.offset == end) {
                end = tok.loc.offset + tok.loc.len;
                tok = lexer_next(&lexer);
            }
            text = (Str){src.data + value.loc.offset, end - value.loc.offset};
            if (value.kind == TOK_NEWLINE || value.kind == TOK_SEMI || value.kind == TOK_EOF) break;
        } else if (value.kind != (is_target ? TOK_IDENT : TOK_STRING)) {
            break;
        }
        char **slot = is_target ? &settings.target : &settings.board;
        /* The first of two is the one checking reports the second against. */
        if (!*slot) *slot = copy_text(text);
    }
    arena_release(&arena);
    return settings;
}

void file_settings_release(FileSettings *settings) {
    free(settings->target);
    free(settings->board);
    memset(settings, 0, sizeof *settings);
}
