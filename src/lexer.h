#ifndef WIND_LEXER_H
#define WIND_LEXER_H

#include "common.h"
#include "types.h"

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_SEMI,
    TOK_IDENT,
    TOK_STRING,
    TOK_INT,
    TOK_FLOAT,
    TOK_CHAR,
    TOK_TYPE,
    TOK_KW_CONST,
    TOK_KW_COUT,
    TOK_KW_CIN,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_DO,
    TOK_KW_FOR,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_GOTO,
    TOK_KW_RETURN,
    TOK_KW_TRUE,
    TOK_KW_FALSE,
    TOK_KW_SIZEOF,
    TOK_KW_UNSIGNED,
    TOK_KW_SIGNED,
    TOK_KW_LONG,
    TOK_KW_SHORT,
    TOK_ASSIGN,
    TOK_SHL,
    TOK_SHR,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_INC,
    TOK_DEC,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_PERCENT_EQ,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_AMP_EQ,
    TOK_PIPE_EQ,
    TOK_CARET_EQ,
    TOK_SHL_EQ,
    TOK_SHR_EQ,
} TokenKind;

typedef struct {
    TokenKind kind;
    SrcLoc loc;
    /* TOK_IDENT: the name, a slice of the source.
       TOK_STRING: the decoded bytes, escapes already resolved.
       TOK_FLOAT: the literal's digits with separators removed. */
    Str text;
    TypeKind type;  /* TOK_TYPE */
    U128 int_value; /* TOK_INT: the value; TOK_CHAR: the byte */
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

/* The reserved words, type names included. */
size_t keyword_count(void);
const char *keyword_name(size_t index);

#endif
