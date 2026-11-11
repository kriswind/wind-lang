#include "lexer.h"

#include <ctype.h>
#include <string.h>

static const struct {
    const char *text;
    TokenKind kind;
    TypeKind type;
} keywords[] = {
    {"const", TOK_KW_CONST, TYPE_INVALID},
    {"cout", TOK_KW_COUT, TYPE_INVALID},
    {"cin", TOK_KW_CIN, TYPE_INVALID},
    {"if", TOK_KW_IF, TYPE_INVALID},
    {"else", TOK_KW_ELSE, TYPE_INVALID},
    {"goto", TOK_KW_GOTO, TYPE_INVALID},
    {"return", TOK_KW_RETURN, TYPE_INVALID},
    {"true", TOK_KW_TRUE, TYPE_INVALID},
    {"false", TOK_KW_FALSE, TYPE_INVALID},
    {"sizeof", TOK_KW_SIZEOF, TYPE_INVALID},
    {"void", TOK_TYPE, TYPE_VOID},
    {"bool", TOK_TYPE, TYPE_BOOL},
    {"string", TOK_TYPE, TYPE_STRING},
    {"char", TOK_TYPE, TYPE_CHAR},
    {"int", TOK_TYPE, TYPE_INT},
    {"i8", TOK_TYPE, TYPE_I8},
    {"i16", TOK_TYPE, TYPE_I16},
    {"i32", TOK_TYPE, TYPE_I32},
    {"i64", TOK_TYPE, TYPE_I64},
    {"i128", TOK_TYPE, TYPE_I128},
    {"uint", TOK_TYPE, TYPE_UINT},
    {"u8", TOK_TYPE, TYPE_U8},
    {"u16", TOK_TYPE, TYPE_U16},
    {"u32", TOK_TYPE, TYPE_U32},
    {"u64", TOK_TYPE, TYPE_U64},
    {"u128", TOK_TYPE, TYPE_U128},
    {"uintptr", TOK_TYPE, TYPE_UINTPTR},
    {"byte", TOK_TYPE, TYPE_U8},
    {"float", TOK_TYPE, TYPE_FLOAT},
    {"f16", TOK_TYPE, TYPE_F16},
    {"f32", TOK_TYPE, TYPE_F32},
    {"f64", TOK_TYPE, TYPE_F64},
    {"f128", TOK_TYPE, TYPE_F128},

    /* C spellings. "unsigned", "signed", "long" and "short" combine with each
       other and with "int", "char" and "double"; the parser resolves them. */
    {"unsigned", TOK_KW_UNSIGNED, TYPE_INVALID},
    {"signed", TOK_KW_SIGNED, TYPE_INVALID},
    {"long", TOK_KW_LONG, TYPE_INVALID},
    {"short", TOK_KW_SHORT, TYPE_INVALID},
    {"double", TOK_TYPE, TYPE_F64},
    {"int8_t", TOK_TYPE, TYPE_I8},
    {"int16_t", TOK_TYPE, TYPE_I16},
    {"int32_t", TOK_TYPE, TYPE_I32},
    {"int64_t", TOK_TYPE, TYPE_I64},
    {"uint8_t", TOK_TYPE, TYPE_U8},
    {"uint16_t", TOK_TYPE, TYPE_U16},
    {"uint32_t", TOK_TYPE, TYPE_U32},
    {"uint64_t", TOK_TYPE, TYPE_U64},
    {"intptr_t", TOK_TYPE, TYPE_INT},
    {"uintptr_t", TOK_TYPE, TYPE_UINTPTR},
    {"ptrdiff_t", TOK_TYPE, TYPE_INT},
    {"ssize_t", TOK_TYPE, TYPE_INT},
    {"size_t", TOK_TYPE, TYPE_UINT},
};

size_t keyword_count(void) {
    return sizeof keywords / sizeof keywords[0];
}

const char *keyword_name(size_t index) {
    return keywords[index].text;
}

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
    case TOK_IDENT:     return "a name";
    case TOK_STRING:    return "string literal";
    case TOK_INT:       return "integer literal";
    case TOK_FLOAT:     return "float literal";
    case TOK_CHAR:      return "character literal";
    case TOK_TYPE:      return "type name";
    case TOK_KW_CONST:  return "'const'";
    case TOK_KW_COUT:   return "'cout'";
    case TOK_KW_CIN:    return "'cin'";
    case TOK_KW_IF:     return "'if'";
    case TOK_KW_ELSE:   return "'else'";
    case TOK_KW_GOTO:   return "'goto'";
    case TOK_KW_RETURN: return "'return'";
    case TOK_KW_TRUE:   return "'true'";
    case TOK_KW_FALSE:  return "'false'";
    case TOK_KW_SIZEOF: return "'sizeof'";
    case TOK_KW_UNSIGNED: return "'unsigned'";
    case TOK_KW_SIGNED: return "'signed'";
    case TOK_KW_LONG:   return "'long'";
    case TOK_KW_SHORT:  return "'short'";
    case TOK_ASSIGN:    return "'='";
    case TOK_SHL:       return "'<<'";
    case TOK_SHR:       return "'>>'";
    case TOK_LPAREN:    return "'('";
    case TOK_RPAREN:    return "')'";
    case TOK_LBRACE:    return "'{'";
    case TOK_RBRACE:    return "'}'";
    case TOK_LBRACKET:  return "'['";
    case TOK_RBRACKET:  return "']'";
    case TOK_COMMA:     return "','";
    case TOK_COLON:     return "':'";
    case TOK_PLUS:      return "'+'";
    case TOK_MINUS:     return "'-'";
    case TOK_STAR:      return "'*'";
    case TOK_SLASH:     return "'/'";
    case TOK_PERCENT:   return "'%'";
    case TOK_EQ:        return "'=='";
    case TOK_NE:        return "'!='";
    case TOK_LT:        return "'<'";
    case TOK_LE:        return "'<='";
    case TOK_GT:        return "'>'";
    case TOK_GE:        return "'>='";
    case TOK_AND:       return "'&&'";
    case TOK_OR:        return "'||'";
    case TOK_NOT:       return "'!'";
    case TOK_INC:       return "'++'";
    case TOK_DEC:       return "'--'";
    case TOK_PLUS_EQ:   return "'+='";
    case TOK_MINUS_EQ:  return "'-='";
    case TOK_STAR_EQ:   return "'*='";
    case TOK_SLASH_EQ:  return "'/='";
    case TOK_PERCENT_EQ: return "'%='";
    }
    return "token";
}

static bool at_end(const Lexer *lexer) {
    return lexer->pos >= lexer->src.len;
}

static char peek_at(const Lexer *lexer, size_t ahead) {
    size_t pos = lexer->pos + ahead;
    return pos < lexer->src.len ? lexer->src.data[pos] : '\0';
}

static char peek(const Lexer *lexer) {
    return peek_at(lexer, 0);
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

static bool match(Lexer *lexer, char c) {
    if (at_end(lexer) || peek(lexer) != c) return false;
    advance(lexer);
    return true;
}

static SrcLoc here(const Lexer *lexer) {
    return (SrcLoc){lexer->pos, 0, lexer->line, lexer->col};
}

/* Newlines outside comments are significant (they end statements), so they
   are not skipped. A block comment is plain whitespace, even when it spans
   lines, and does not nest. */
static void skip_blanks_and_comments(Lexer *lexer) {
    while (!at_end(lexer)) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '/' && peek_at(lexer, 1) == '/') {
            while (!at_end(lexer) && peek(lexer) != '\n') advance(lexer);
        } else if (c == '/' && peek_at(lexer, 1) == '*') {
            SrcLoc start = here(lexer);
            start.len = 2;
            advance(lexer);
            advance(lexer);
            for (;;) {
                if (at_end(lexer)) {
                    diag_add(lexer->diags, SEV_ERROR, start, "unterminated comment");
                    return;
                }
                if (advance(lexer) == '*' && match(lexer, '/')) break;
            }
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
    case '\'': return '\'';
    }
    if (isprint((unsigned char)c)) {
        diag_add(lexer->diags, SEV_ERROR, loc, "unknown escape sequence '\\%c'", c);
    } else {
        diag_add(lexer->diags, SEV_ERROR, loc, "unknown escape sequence");
    }
    return c;
}

static bool at_line_end(const Lexer *lexer) {
    return at_end(lexer) || peek(lexer) == '\n';
}

/* Called with the opening quote already consumed. An unterminated literal is
   reported and yields the bytes up to the end of the line. */
static Str lex_string_body(Lexer *lexer, SrcLoc start) {
    /* A decoded string is never longer than its source spelling. */
    char *buf = arena_alloc(lexer->arena, lexer->src.len - lexer->pos);
    size_t len = 0;
    for (;;) {
        if (at_line_end(lexer)) {
            start.len = lexer->pos - start.offset;
            diag_add(lexer->diags, SEV_ERROR, start, "unterminated string literal");
            break;
        }
        char c = advance(lexer);
        if (c == '"') break;
        if (c == '\\' && !at_line_end(lexer)) c = decode_escape(lexer);
        buf[len++] = c;
    }
    return (Str){buf, len};
}

/* Called with the opening quote already consumed. A char is one byte, so a
   literal holding more (or a multi-byte UTF-8 character) is an error. */
static char lex_char_body(Lexer *lexer, SrcLoc start) {
    char value = 0;
    size_t count = 0;
    for (;;) {
        if (at_line_end(lexer)) {
            start.len = lexer->pos - start.offset;
            diag_add(lexer->diags, SEV_ERROR, start, "unterminated character literal");
            return value;
        }
        char c = advance(lexer);
        if (c == '\'') break;
        if (c == '\\' && !at_line_end(lexer)) c = decode_escape(lexer);
        if (count++ == 0) value = c;
    }
    start.len = lexer->pos - start.offset;
    if (count == 0) {
        diag_add(lexer->diags, SEV_ERROR, start, "empty character literal");
    } else if (count > 1) {
        diag_add(lexer->diags, SEV_ERROR, start,
                 "character literal must be a single byte; use a string for more");
    }
    return value;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

static bool is_digit_or_separator(char c) {
    return isdigit((unsigned char)c) || c == '_';
}

/* C's integer suffixes: u, l, ll and their combinations, in either case. */
static bool is_int_suffix(const char *text, size_t len) {
    size_t u = 0, l = 0;
    for (size_t i = 0; i < len; i++) {
        char c = (char)tolower((unsigned char)text[i]);
        if (c == 'u') u++;
        else if (c == 'l') l++;
        else return false;
    }
    return u <= 1 && l <= 2;
}

/*
 * Called with the first digit already consumed. Accepts:
 *
 *   decimal  42  1_000_000
 *   prefixed 0xFF  0b1010  0o17
 *   float    1.5  2e10  6.02e+23
 *
 * C's type suffixes (1.5f, 10UL) are accepted and ignored: a literal takes
 * its type from where it is used. A malformed literal is reported and yields
 * the integer 0.
 */
static void lex_number(Lexer *lexer, Token *tok) {
    const char *text = lexer->src.data + tok->loc.offset;
    int base = 10;
    bool is_float = false;
    bool valid = true;
    size_t digits_len; /* the literal without its suffix */

    if (text[0] == '0' && peek(lexer) != '\0' && strchr("xXbBoO", peek(lexer))) {
        char prefix = (char)tolower((unsigned char)advance(lexer));
        base = prefix == 'x' ? 16 : prefix == 'b' ? 2 : 8;
        while (is_ident_char(peek(lexer))) advance(lexer);
        digits_len = lexer->pos - tok->loc.offset;
        while (digits_len > 2 && strchr("uUlL", text[digits_len - 1])) digits_len--;
    } else {
        while (is_digit_or_separator(peek(lexer))) advance(lexer);
        if (peek(lexer) == '.' && isdigit((unsigned char)peek_at(lexer, 1))) {
            is_float = true;
            advance(lexer);
            while (is_digit_or_separator(peek(lexer))) advance(lexer);
        }
        if (peek(lexer) == 'e' || peek(lexer) == 'E') {
            size_t sign = (peek_at(lexer, 1) == '+' || peek_at(lexer, 1) == '-') ? 1 : 0;
            if (isdigit((unsigned char)peek_at(lexer, 1 + sign))) {
                is_float = true;
                for (size_t i = 0; i < 1 + sign; i++) advance(lexer);
                while (isdigit((unsigned char)peek(lexer))) advance(lexer);
            }
        }
        digits_len = lexer->pos - tok->loc.offset;
        while (is_ident_char(peek(lexer))) advance(lexer);
        const char *suffix = text + digits_len;
        size_t suffix_len = lexer->pos - tok->loc.offset - digits_len;
        if (is_float) {
            valid = suffix_len == 0 || (suffix_len == 1 && strchr("fFlL", suffix[0]));
        } else {
            valid = is_int_suffix(suffix, suffix_len);
        }
    }

    tok->loc.len = lexer->pos - tok->loc.offset;
    tok->kind = TOK_INT;
    tok->int_value = 0;

    if (is_float && valid) {
        char *digits = arena_alloc(lexer->arena, digits_len);
        size_t count = 0;
        for (size_t i = 0; i < digits_len; i++) {
            if (text[i] != '_') digits[count++] = text[i];
        }
        tok->kind = TOK_FLOAT;
        tok->text = (Str){digits, count};
        return;
    }

    bool too_large = false;
    bool any_digit = false;
    U128 value = 0;
    for (size_t i = base == 10 ? 0 : 2; valid && i < digits_len; i++) {
        if (text[i] == '_') continue;
        int digit = digit_value(text[i]);
        if (digit >= base) {
            valid = false;
        } else if (value > (~(U128)0 - (U128)digit) / (U128)base) {
            too_large = true;
        } else {
            value = value * (U128)base + (U128)digit;
        }
        any_digit = true;
    }
    if (!valid || !any_digit) {
        diag_add(lexer->diags, SEV_ERROR, tok->loc, "invalid number literal '%.*s'",
                 (int)tok->loc.len, text);
    } else if (too_large) {
        diag_add(lexer->diags, SEV_ERROR, tok->loc, "integer literal is too large");
    } else {
        tok->int_value = value;
    }
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
            for (size_t i = 0; i < keyword_count(); i++) {
                if (str_eq_cstr(tok.text, keywords[i].text)) {
                    tok.kind = keywords[i].kind;
                    tok.type = keywords[i].type;
                    break;
                }
            }
            return tok;
        }

        if (isdigit((unsigned char)c)) {
            lex_number(lexer, &tok);
            return tok;
        }

        switch (c) {
        case ';': tok.kind = TOK_SEMI;    break;
        case '(': tok.kind = TOK_LPAREN;  break;
        case ')': tok.kind = TOK_RPAREN;  break;
        case '{': tok.kind = TOK_LBRACE;  break;
        case '}': tok.kind = TOK_RBRACE;  break;
        case '[': tok.kind = TOK_LBRACKET; break;
        case ']': tok.kind = TOK_RBRACKET; break;
        case ',': tok.kind = TOK_COMMA;   break;
        case ':': tok.kind = TOK_COLON;   break;
        case '+':
            tok.kind = match(lexer, '+') ? TOK_INC : match(lexer, '=') ? TOK_PLUS_EQ : TOK_PLUS;
            break;
        case '-':
            tok.kind = match(lexer, '-') ? TOK_DEC : match(lexer, '=') ? TOK_MINUS_EQ : TOK_MINUS;
            break;
        case '*': tok.kind = match(lexer, '=') ? TOK_STAR_EQ : TOK_STAR;       break;
        case '/': tok.kind = match(lexer, '=') ? TOK_SLASH_EQ : TOK_SLASH;     break;
        case '%': tok.kind = match(lexer, '=') ? TOK_PERCENT_EQ : TOK_PERCENT; break;
        case '=': tok.kind = match(lexer, '=') ? TOK_EQ : TOK_ASSIGN; break;
        case '!': tok.kind = match(lexer, '=') ? TOK_NE : TOK_NOT;    break;
        case '>':
            tok.kind = match(lexer, '>') ? TOK_SHR : match(lexer, '=') ? TOK_GE : TOK_GT;
            break;
        case '<':
            tok.kind = match(lexer, '<') ? TOK_SHL : match(lexer, '=') ? TOK_LE : TOK_LT;
            break;
        case '"':
            tok.kind = TOK_STRING;
            tok.text = lex_string_body(lexer, tok.loc);
            break;
        case '\'':
            tok.kind = TOK_CHAR;
            tok.int_value = (unsigned char)lex_char_body(lexer, tok.loc);
            break;
        case '&':
        case '|':
            if (match(lexer, c)) {
                tok.kind = c == '&' ? TOK_AND : TOK_OR;
                break;
            }
            tok.loc.len = 1;
            diag_add(lexer->diags, SEV_ERROR, tok.loc,
                     "unexpected character '%c' (did you mean '%c%c'?)", c, c, c);
            continue;
        default:
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
