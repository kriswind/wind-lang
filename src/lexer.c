#include "lexer.h"

#include <ctype.h>

const char *const wind_keywords[] = {"const", "string", "cout"};
const size_t wind_keyword_count = sizeof wind_keywords / sizeof wind_keywords[0];

static const TokenKind keyword_kinds[] = {
    TOK_KW_CONST,
    TOK_KW_STRING,
    TOK_KW_COUT,
};

void lexer_init(Lexer *lexer, Str src, Arena *arena, Diags *diags) {
    lexer->src = src;
    lexer->pos = 0;
    lexer->line = 1;
    lexer->col = 1;
    lexer->arena = arena;
    lexer->diags = diags;
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TOK_EOF:       return "end of file";
    case TOK_NEWLINE:   return "end of line";
    case TOK_SEMI:      return "';'";
    case TOK_IDENT:     return "identifier";
    case TOK_STRING:    return "string literal";
    case TOK_KW_CONST:  return "'const'";
    case TOK_KW_STRING: return "'string'";
    case TOK_KW_COUT:   return "'cout'";
    case TOK_ASSIGN:    return "'='";
    case TOK_SHL:       return "'<<'";
    }
    return "token";
}

static bool at_end(const Lexer *lexer) {
    return lexer->pos >= lexer->src.len;
}

static char peek(const Lexer *lexer) {
    return at_end(lexer) ? '\0' : lexer->src.data[lexer->pos];
}

static char advance(Lexer *lexer) {
    char c = lexer->src.data[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->col = 1;
    } else {
        lexer->col++;
    }
    return c;
}

static SrcLoc here(const Lexer *lexer) {
    return (SrcLoc){lexer->pos, 0, lexer->line, lexer->col};
}

/* Newlines are significant (they end statements), so they are not skipped. */
static void skip_blanks_and_comments(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        if (at_end(lexer)) return;
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '/' && lexer->pos + 1 < lexer->src.len &&
                   lexer->src.data[lexer->pos + 1] == '/') {
            while (!at_end(lexer) && peek(lexer) != '\n') advance(lexer);
        } else {
            return;
        }
    }
}

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_utf8_continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

/* Called with the backslash already consumed and a character available. */
static char decode_escape(Lexer *lexer) {
    SrcLoc loc = here(lexer);
    loc.offset -= 1;
    loc.col -= 1;
    loc.len = 2;
    char c = advance(lexer);
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '0':  return '\0';
    case '\\': return '\\';
    case '"':  return '"';
    }
    if (isprint((unsigned char)c)) {
        diag_add(lexer->diags, SEV_ERROR, loc, "unknown escape sequence '\\%c'", c);
    } else {
        diag_add(lexer->diags, SEV_ERROR, loc, "unknown escape sequence");
    }
    return c;
}

/* Called with the opening quote already consumed. An unterminated literal is
   reported and yields the bytes up to the end of the line. */
static Str lex_string_body(Lexer *lexer, SrcLoc start) {
    /* A decoded string is never longer than its source spelling. */
    char *buf = arena_alloc(lexer->arena, lexer->src.len - lexer->pos);
    size_t len = 0;
    for (;;) {
        if (at_end(lexer) || peek(lexer) == '\n') {
            start.len = lexer->pos - start.offset;
            diag_add(lexer->diags, SEV_ERROR, start, "unterminated string literal");
            break;
        }
        char c = advance(lexer);
        if (c == '"') break;
        if (c == '\\' && !at_end(lexer) && peek(lexer) != '\n') {
            c = decode_escape(lexer);
        }
        buf[len++] = c;
    }
    return (Str){buf, len};
}

Token lexer_next(Lexer *lexer) {
    for (;;) {
        skip_blanks_and_comments(lexer);

        Token tok = {.kind = TOK_EOF, .loc = here(lexer)};
        if (at_end(lexer)) return tok;

        char c = advance(lexer);

        if (c == '\n') {
            tok.kind = TOK_NEWLINE;
            return tok;
        }

        if (is_ident_start(c)) {
            while (is_ident_char(peek(lexer))) advance(lexer);
            tok.kind = TOK_IDENT;
            tok.loc.len = lexer->pos - tok.loc.offset;
            tok.text = (Str){lexer->src.data + tok.loc.offset, tok.loc.len};
            for (size_t i = 0; i < wind_keyword_count; i++) {
                if (str_eq_cstr(tok.text, wind_keywords[i])) {
                    tok.kind = keyword_kinds[i];
                    break;
                }
            }
            return tok;
        }

        switch (c) {
        case ';': tok.kind = TOK_SEMI;   break;
        case '=': tok.kind = TOK_ASSIGN; break;
        case '"':
            tok.kind = TOK_STRING;
            tok.text = lex_string_body(lexer, tok.loc);
            break;
        case '<':
            if (peek(lexer) == '<') {
                advance(lexer);
                tok.kind = TOK_SHL;
                break;
            }
            tok.loc.len = 1;
            diag_add(lexer->diags, SEV_ERROR, tok.loc,
                     "unexpected character '<' (did you mean '<<'?)");
            continue;
        default:
            if (isdigit((unsigned char)c)) {
                while (is_ident_char(peek(lexer))) advance(lexer);
                tok.loc.len = lexer->pos - tok.loc.offset;
                diag_add(lexer->diags, SEV_ERROR, tok.loc, "unexpected number '%.*s'",
                         (int)tok.loc.len, lexer->src.data + tok.loc.offset);
                continue;
            }
            /* A multi-byte UTF-8 character is reported once, as a whole. */
            while (is_utf8_continuation(peek(lexer))) advance(lexer);
            tok.loc.len = lexer->pos - tok.loc.offset;
            if (isprint((unsigned char)c)) {
                diag_add(lexer->diags, SEV_ERROR, tok.loc,
                         "unexpected character '%c'", c);
            } else {
                diag_add(lexer->diags, SEV_ERROR, tok.loc, "unexpected character");
            }
            continue;
        }
        tok.loc.len = lexer->pos - tok.loc.offset;
        return tok;
    }
}
