#ifndef WIND_LEXER_H
#define WIND_LEXER_H

#include "common.h"

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_SEMI,
    TOK_IDENT,
    TOK_STRING,
    TOK_KW_CONST,
    TOK_KW_STRING,
    TOK_KW_COUT,
    TOK_ASSIGN,
    TOK_SHL,
} TokenKind;

typedef struct {
    TokenKind kind;
    SrcLoc loc;
    /* TOK_IDENT: the name, a slice of the source.
       TOK_STRING: the decoded bytes, escapes already resolved. */
    Str text;
} Token;

typedef struct {
    Str src;
    size_t pos;
    int line;
    int col;
    Arena *arena;
    Diags *diags;
} Lexer;

void lexer_init(Lexer *lexer, Str src, Arena *arena, Diags *diags);

/* Never fails: malformed input is reported to the lexer's Diags and skipped
   or repaired, so the caller always receives a well-formed token. */
Token lexer_next(Lexer *lexer);

const char *token_kind_name(TokenKind kind);

extern const char *const wind_keywords[];
extern const size_t wind_keyword_count;

#endif
