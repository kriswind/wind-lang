#define _POSIX_C_SOURCE 200809L /* open_memstream */

#include "codegen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Shape of the generated C:
 *
 * - Top-level statements become the body of the C main(), which then calls
 *   the Wind main if there is one. Functions become static C functions.
 * - Variables declared directly at the top level are static objects. Every
 *   other variable is declared at the top of its C function. Either way it
 *   starts zeroed and its Wind declaration becomes an assignment, so a goto
 *   can never jump past an initialization into undefined state.
 * - Non-string constants are substituted at each use. String constants are
 *   static objects.
 * - Arithmetic results are cast back to their Wind type, undoing C's integer
 *   promotion so that narrow types wrap at their own width.
 *
 * - Only the runtime support a program uses is emitted. The program is
 *   generated first, into memory, recording each runtime type and helper it
 *   mentions; the matching definitions are then written ahead of it.
 *
 * Generated names are prefixed by kind and, for locals, numbered, so no two
 * Wind names can meet in C.
 */

/* ---- Runtime support ---- */

/* Listed so that each helper comes after the ones its definition calls. */
typedef enum {
    HELPER_NONE, /* ends a helper's list of needs */
    HELPER_CIN_STATE,
    HELPER_IS_SPACE,
    HELPER_READ_WORD,
    HELPER_PARSE_U128,
    HELPER_READ_STRING,
    HELPER_READ_BOOL,
    HELPER_READ_CHAR,
    HELPER_READ_INT,
    HELPER_READ_UINT,
    HELPER_READ_U128,
    HELPER_READ_I128,
    HELPER_READ_FLOAT,
    HELPER_WRITE_STRING,
    HELPER_STRING_EQ,
    HELPER_WRITE_BOOL,
    HELPER_WRITE_CHAR,
    HELPER_WRITE_INT,
    HELPER_WRITE_UINT,
    HELPER_WRITE_U128,
    HELPER_WRITE_I128,
    HELPER_FLOAT_WRITER,
    HELPER_WRITE_F16,
    HELPER_WRITE_F32,
    HELPER_WRITE_F64,
    HELPER_WRITE_F128,
    HELPER_COUNT,
} Helper;

typedef enum {
    HEADER_ERRNO = 1 << 0,
    HEADER_STDDEF = 1 << 1,
    HEADER_STDINT = 1 << 2,
    HEADER_STDIO = 1 << 3,
    HEADER_STDLIB = 1 << 4,
    HEADER_STRING = 1 << 5,
} Header;

/* What the program generated so far needs from the runtime. */
static struct {
    bool types[TYPE_COUNT];
    bool helpers[HELPER_COUNT];
} used;

/* The C name of a type, noted as needing its definition. */
static const char *c_type(TypeKind type) {
    used.types[type] = true;
    return type_c_name(type);
}

static const char *const type_definitions[TYPE_COUNT] = {
    [TYPE_STRING] = "typedef struct {\n"
                    "    const char *data;\n"
                    "    size_t len;\n"
                    "} wind_string;\n",
    [TYPE_BOOL] = "typedef _Bool wind_bool;\n",
    [TYPE_CHAR] = "typedef unsigned char wind_char;\n",
    [TYPE_INT] = "typedef intptr_t wind_int;\n",
    [TYPE_I8] = "typedef int8_t wind_i8;\n",
    [TYPE_I16] = "typedef int16_t wind_i16;\n",
    [TYPE_I32] = "typedef int32_t wind_i32;\n",
    [TYPE_I64] = "typedef int64_t wind_i64;\n",
    [TYPE_I128] = "typedef __int128 wind_i128;\n",
    [TYPE_UINT] = "typedef uintptr_t wind_uint;\n",
    [TYPE_U8] = "typedef uint8_t wind_u8;\n",
    [TYPE_U16] = "typedef uint16_t wind_u16;\n",
    [TYPE_U32] = "typedef uint32_t wind_u32;\n",
    [TYPE_U64] = "typedef uint64_t wind_u64;\n",
    [TYPE_U128] = "typedef unsigned __int128 wind_u128;\n",
    [TYPE_UINTPTR] = "typedef uintptr_t wind_uintptr;\n",
    [TYPE_FLOAT] = "typedef double wind_float;\n",
    [TYPE_F32] = "typedef float wind_f32;\n",
    [TYPE_F64] = "typedef double wind_f64;\n",
    /* The two types below fall back to the nearest standard type on
       compilers without the extension. */
    [TYPE_F16] = "#ifdef __FLT16_MANT_DIG__\n"
                 "typedef _Float16 wind_f16;\n"
                 "#else\n"
                 "typedef float wind_f16;\n"
                 "#endif\n",
    [TYPE_F128] = "#ifdef __SIZEOF_FLOAT128__\n"
                  "typedef __float128 wind_f128;\n"
                  "#else\n"
                  "typedef long double wind_f128;\n"
                  "#endif\n",
};

typedef struct {
    const char *definition;
    unsigned headers;     /* Header flags its definition relies on */
    Helper needs[4];      /* other helpers its definition calls */
    TypeKind types[3];    /* types its definition mentions */
} HelperInfo;

static const HelperInfo helpers[HELPER_COUNT] = {
    [HELPER_CIN_STATE] = {
        "/* Whether the most recent cin read succeeded. */\n"
        "static wind_bool wind_cin_ok = 1;\n",
        0, {0}, {TYPE_BOOL}},
    [HELPER_IS_SPACE] = {
        "static int wind_is_space(int c) {\n"
        "    return c == ' ' || (c >= '\\t' && c <= '\\r');\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_READ_WORD] = {
        "/* Reads the next whitespace-delimited word from stdin into a reused,\n"
        "   NUL-terminated buffer. Returns its length; 0 means the input is\n"
        "   exhausted. Pending output is flushed first, so a prompt is visible. */\n"
        "static size_t wind_read_word(char **word) {\n"
        "    static char *buf;\n"
        "    static size_t cap;\n"
        "    size_t len = 0;\n"
        "    int c;\n"
        "    fflush(stdout);\n"
        "    do {\n"
        "        c = getchar();\n"
        "    } while (wind_is_space(c));\n"
        "    while (c != EOF && !wind_is_space(c)) {\n"
        "        if (len + 1 >= cap) {\n"
        "            cap = cap ? cap * 2 : 64;\n"
        "            buf = realloc(buf, cap);\n"
        "            if (!buf) abort();\n"
        "        }\n"
        "        buf[len++] = (char)c;\n"
        "        c = getchar();\n"
        "    }\n"
        "    if (c != EOF) ungetc(c, stdin);\n"
        "    if (buf) buf[len] = '\\0';\n"
        "    *word = buf;\n"
        "    return len;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDIO | HEADER_STDLIB, {HELPER_IS_SPACE}, {0}},
    [HELPER_PARSE_U128] = {
        "/* Parses decimal digits as a value no greater than max. */\n"
        "static wind_u128 wind_parse_u128(const char *digits, size_t len, wind_u128 max) {\n"
        "    wind_u128 v = 0;\n"
        "    wind_cin_ok = 0;\n"
        "    if (len == 0) return 0;\n"
        "    for (size_t i = 0; i < len; i++) {\n"
        "        if (digits[i] < '0' || digits[i] > '9') return 0;\n"
        "        wind_u128 digit = (wind_u128)(digits[i] - '0');\n"
        "        if (v > (max - digit) / 10) return 0;\n"
        "        v = v * 10 + digit;\n"
        "    }\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        HEADER_STDDEF, {HELPER_CIN_STATE}, {TYPE_U128}},
    [HELPER_READ_STRING] = {
        "/* The word is copied to memory that is never freed. */\n"
        "static wind_string wind_read_string(void) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    wind_string s = {0};\n"
        "    wind_cin_ok = len != 0;\n"
        "    if (len) {\n"
        "        char *copy = malloc(len);\n"
        "        if (!copy) abort();\n"
        "        memcpy(copy, word, len);\n"
        "        s.data = copy;\n"
        "        s.len = len;\n"
        "    }\n"
        "    return s;\n"
        "}\n",
        HEADER_STDLIB | HEADER_STRING, {HELPER_CIN_STATE, HELPER_READ_WORD}, {TYPE_STRING}},
    [HELPER_READ_BOOL] = {
        "static wind_bool wind_read_bool(void) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    wind_bool value = len && (!strcmp(word, \"true\") || !strcmp(word, \"1\"));\n"
        "    wind_cin_ok = value || (len && (!strcmp(word, \"false\") || !strcmp(word, \"0\")));\n"
        "    return value;\n"
        "}\n",
        HEADER_STRING, {HELPER_CIN_STATE, HELPER_READ_WORD}, {TYPE_BOOL}},
    [HELPER_READ_CHAR] = {
        "/* Reads the next byte that is not whitespace. */\n"
        "static wind_char wind_read_char(void) {\n"
        "    int c;\n"
        "    fflush(stdout);\n"
        "    do {\n"
        "        c = getchar();\n"
        "    } while (wind_is_space(c));\n"
        "    wind_cin_ok = c != EOF;\n"
        "    return c == EOF ? 0 : (wind_char)c;\n"
        "}\n",
        HEADER_STDIO, {HELPER_CIN_STATE, HELPER_IS_SPACE}, {TYPE_CHAR}},
    [HELPER_READ_INT] = {
        "/* Reads a decimal integer within [min, max]. */\n"
        "static long long wind_read_int(long long min, long long max) {\n"
        "    char *word, *end;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    wind_cin_ok = 0;\n"
        "    if (len == 0) return 0;\n"
        "    errno = 0;\n"
        "    long long v = strtoll(word, &end, 10);\n"
        "    if (errno || end != word + len || v < min || v > max) return 0;\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        HEADER_ERRNO | HEADER_STDINT | HEADER_STDLIB, {HELPER_CIN_STATE, HELPER_READ_WORD}, {0}},
    [HELPER_READ_UINT] = {
        "/* Reads a decimal integer within [0, max]. */\n"
        "static unsigned long long wind_read_uint(unsigned long long max) {\n"
        "    char *word, *end;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    wind_cin_ok = 0;\n"
        "    if (len == 0 || word[0] == '-') return 0;\n"
        "    errno = 0;\n"
        "    unsigned long long v = strtoull(word, &end, 10);\n"
        "    if (errno || end != word + len || v > max) return 0;\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        HEADER_ERRNO | HEADER_STDINT | HEADER_STDLIB, {HELPER_CIN_STATE, HELPER_READ_WORD}, {0}},
    [HELPER_READ_U128] = {
        "static wind_u128 wind_read_u128(void) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    if (len && word[0] == '+') {\n"
        "        word++;\n"
        "        len--;\n"
        "    }\n"
        "    return wind_parse_u128(word, len, ~(wind_u128)0);\n"
        "}\n",
        0, {HELPER_READ_WORD, HELPER_PARSE_U128}, {TYPE_U128}},
    [HELPER_READ_I128] = {
        "static wind_i128 wind_read_i128(void) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    int negative = len && word[0] == '-';\n"
        "    if (len && (word[0] == '-' || word[0] == '+')) {\n"
        "        word++;\n"
        "        len--;\n"
        "    }\n"
        "    wind_u128 max = ((wind_u128)1 << 127) - (negative ? 0 : 1);\n"
        "    wind_u128 v = wind_parse_u128(word, len, max);\n"
        "    return negative ? (wind_i128)(-v) : (wind_i128)v;\n"
        "}\n",
        0, {HELPER_READ_WORD, HELPER_PARSE_U128}, {TYPE_I128, TYPE_U128}},
    [HELPER_READ_FLOAT] = {
        "/* Read as long double, the widest type the C library parses. */\n"
        "static long double wind_read_float(void) {\n"
        "    char *word, *end;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    wind_cin_ok = 0;\n"
        "    if (len == 0) return 0;\n"
        "    long double v = strtold(word, &end);\n"
        "    if (end != word + len) return 0;\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        HEADER_STDLIB, {HELPER_CIN_STATE, HELPER_READ_WORD}, {0}},
    [HELPER_WRITE_STRING] = {
        "static void wind_write_string(wind_string s) {\n"
        "    if (s.len) fwrite(s.data, 1, s.len, stdout);\n"
        "}\n",
        HEADER_STDIO, {0}, {TYPE_STRING}},
    [HELPER_STRING_EQ] = {
        "static wind_bool wind_string_eq(wind_string a, wind_string b) {\n"
        "    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n"
        "}\n",
        HEADER_STRING, {0}, {TYPE_STRING, TYPE_BOOL}},
    [HELPER_WRITE_BOOL] = {
        "static void wind_write_bool(wind_bool b) {\n"
        "    fputs(b ? \"true\" : \"false\", stdout);\n"
        "}\n",
        HEADER_STDIO, {0}, {TYPE_BOOL}},
    [HELPER_WRITE_CHAR] = {
        "static void wind_write_char(wind_char c) {\n"
        "    fputc(c, stdout);\n"
        "}\n",
        HEADER_STDIO, {0}, {TYPE_CHAR}},
    [HELPER_WRITE_INT] = {
        "static void wind_write_int(long long v) {\n"
        "    printf(\"%lld\", v);\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_WRITE_UINT] = {
        "static void wind_write_uint(unsigned long long v) {\n"
        "    printf(\"%llu\", v);\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_WRITE_U128] = {
        "static void wind_write_u128(wind_u128 v) {\n"
        "    char buf[40];\n"
        "    char *p = buf + sizeof buf;\n"
        "    do {\n"
        "        *--p = (char)('0' + (int)(v % 10));\n"
        "        v /= 10;\n"
        "    } while (v);\n"
        "    fwrite(p, 1, (size_t)(buf + sizeof buf - p), stdout);\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDIO, {0}, {TYPE_U128}},
    [HELPER_WRITE_I128] = {
        "static void wind_write_i128(wind_i128 v) {\n"
        "    if (v < 0) {\n"
        "        fputc('-', stdout);\n"
        "        wind_write_u128(-(wind_u128)v);\n"
        "    } else {\n"
        "        wind_write_u128((wind_u128)v);\n"
        "    }\n"
        "}\n",
        HEADER_STDIO, {HELPER_WRITE_U128}, {TYPE_I128, TYPE_U128}},
    [HELPER_FLOAT_WRITER] = {
        "/* Defines the function that prints one float type. Prints the fewest\n"
        "   digits that read back as the same value; values are formatted through\n"
        "   long double, the widest type printf accepts. */\n"
        "#define WIND_WRITE_FLOAT(name, T, max_digits, parse) \\\n"
        "    static void name(T v) { \\\n"
        "        char buf[64]; \\\n"
        "        T shown = (T)(long double)v; \\\n"
        "        for (int digits = 1; digits <= max_digits; digits++) { \\\n"
        "            snprintf(buf, sizeof buf, \"%.*Lg\", digits, (long double)v); \\\n"
        "            if ((T)parse(buf, NULL) == shown) break; \\\n"
        "        } \\\n"
        "        fputs(buf, stdout); \\\n"
        "    }\n",
        HEADER_STDIO | HEADER_STDLIB, {0}, {0}},
    [HELPER_WRITE_F16] = {"WIND_WRITE_FLOAT(wind_write_f16, wind_f16, 5, strtof)\n", 0,
                          {HELPER_FLOAT_WRITER}, {TYPE_F16}},
    [HELPER_WRITE_F32] = {"WIND_WRITE_FLOAT(wind_write_f32, wind_f32, 9, strtof)\n", 0,
                          {HELPER_FLOAT_WRITER}, {TYPE_F32}},
    [HELPER_WRITE_F64] = {"WIND_WRITE_FLOAT(wind_write_f64, wind_f64, 17, strtod)\n", 0,
                          {HELPER_FLOAT_WRITER}, {TYPE_F64}},
    [HELPER_WRITE_F128] = {"WIND_WRITE_FLOAT(wind_write_f128, wind_f128, 21, strtold)\n", 0,
                           {HELPER_FLOAT_WRITER}, {TYPE_F128}},
};

/* Writes the headers, type definitions and helpers recorded in `used`. */
static void emit_runtime(FILE *out) {
    /* A helper can need another that sits anywhere in the list, so the set
       is grown until it stops changing. */
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < HELPER_COUNT; i++) {
            if (!used.helpers[i]) continue;
            for (size_t j = 0; j < 4 && helpers[i].needs[j] != HELPER_NONE; j++) {
                if (!used.helpers[helpers[i].needs[j]]) {
                    used.helpers[helpers[i].needs[j]] = true;
                    changed = true;
                }
            }
        }
    }

    unsigned headers = 0;
    for (int i = 0; i < HELPER_COUNT; i++) {
        if (!used.helpers[i]) continue;
        headers |= helpers[i].headers;
        for (size_t j = 0; j < 3 && helpers[i].types[j] != TYPE_INVALID; j++) {
            used.types[helpers[i].types[j]] = true;
        }
    }
    if (used.types[TYPE_STRING]) headers |= HEADER_STDDEF;
    for (int type = TYPE_INT; type <= TYPE_UINTPTR; type++) {
        if (used.types[type] && type != TYPE_I128 && type != TYPE_U128) {
            headers |= HEADER_STDINT;
        }
    }

    static const struct {
        Header header;
        const char *name;
    } header_names[] = {
        {HEADER_ERRNO, "errno.h"},   {HEADER_STDDEF, "stddef.h"},
        {HEADER_STDINT, "stdint.h"}, {HEADER_STDIO, "stdio.h"},
        {HEADER_STDLIB, "stdlib.h"}, {HEADER_STRING, "string.h"},
    };
    for (size_t i = 0; i < sizeof header_names / sizeof header_names[0]; i++) {
        if (headers & header_names[i].header) {
            fprintf(out, "#include <%s>\n", header_names[i].name);
        }
    }
    if (headers) fputs("\n", out);

    bool any = false;
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.types[type] || !type_definitions[type]) continue;
        fputs(type_definitions[type], out);
        any = true;
    }
    if (any) fputs("\n", out);

    for (int i = 0; i < HELPER_COUNT; i++) {
        if (used.helpers[i]) fprintf(out, "%s\n", helpers[i].definition);
    }
}

/* ---- Program ---- */

static void emit_expr(const Expr *expr, FILE *out);

/* Non-printable bytes use three-digit octal escapes, which cannot absorb a
   following digit the way hex escapes do. '?' is escaped to rule out
   trigraphs. */
static void emit_c_string(Str s, FILE *out) {
    fputc('"', out);
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '"' || c == '\\' || c == '?') {
            fprintf(out, "\\%c", c);
        } else if (c >= 0x20 && c < 0x7f) {
            fputc(c, out);
        } else {
            fprintf(out, "\\%03o", c);
        }
    }
    fputc('"', out);
}

static void emit_symbol(const Symbol *symbol, FILE *out) {
    int len = (int)symbol->name.len;
    const char *name = symbol->name.data;
    switch (symbol->kind) {
    case SYM_FUNC:  fprintf(out, "wind_f_%.*s", len, name); break;
    case SYM_LABEL: fprintf(out, "wind_l_%.*s", len, name); break;
    case SYM_CONST: fprintf(out, "wind_c%d_%.*s", symbol->id, len, name); break;
    case SYM_VAR:
    case SYM_PARAM: fprintf(out, "wind_v%d_%.*s", symbol->id, len, name); break;
    }
}

/* The string literal a string constant ultimately names, following
   constant-to-constant initializers. */
static Str const_string(const Symbol *symbol) {
    const Expr *expr = symbol->decl->as.decl.init;
    while (expr->kind == EXPR_IDENT) expr = expr->as.ident.symbol->decl->as.decl.init;
    return expr->as.string_lit;
}

static void emit_int_literal(const Expr *expr, FILE *out) {
    U128 value = expr->as.int_lit;
    uint64_t low = (uint64_t)value;
    uint64_t high = (uint64_t)(value >> 64);
    fprintf(out, "((%s)", c_type(expr->type));
    if (high == 0) {
        fprintf(out, "%lluULL)", (unsigned long long)low);
    } else {
        fprintf(out, "(((%s)%lluULL << 64) | %lluULL))", c_type(TYPE_U128),
                (unsigned long long)high,
                (unsigned long long)low);
    }
}

static void emit_float_literal(const Expr *expr, FILE *out) {
    /* The suffix makes C read the digits at the precision of the target
       type instead of rounding through double. */
    const char *suffix = "";
    if (expr->type == TYPE_F16 || expr->type == TYPE_F32) suffix = "f";
    if (expr->type == TYPE_F128) suffix = "L";
    fprintf(out, "((%s)%.*s%s)", c_type(expr->type), (int)expr->as.float_lit.len,
            expr->as.float_lit.data, suffix);
}

static void emit_binary(const Expr *expr, FILE *out) {
    Op op = expr->as.binary.op;
    const Expr *left = expr->as.binary.left;
    const Expr *right = expr->as.binary.right;

    if (left->type == TYPE_STRING) {
        used.helpers[HELPER_STRING_EQ] = true;
        fputs(op == OP_NE ? "(!wind_string_eq(" : "(wind_string_eq(", out);
        emit_expr(left, out);
        fputs(", ", out);
        emit_expr(right, out);
        fputs("))", out);
        return;
    }
    fprintf(out, "((%s)(", c_type(expr->type));
    emit_expr(left, out);
    fprintf(out, " %s ", op_text(op));
    emit_expr(right, out);
    fputs("))", out);
}

static void emit_expr(const Expr *expr, FILE *out) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
        emit_int_literal(expr, out);
        break;
    case EXPR_FLOAT_LIT:
        emit_float_literal(expr, out);
        break;
    case EXPR_CHAR_LIT:
        fprintf(out, "((%s)%d)", c_type(TYPE_CHAR), (unsigned char)expr->as.char_lit);
        break;
    case EXPR_STRING_LIT:
        fprintf(out, "(%s){ ", c_type(TYPE_STRING));
        emit_c_string(expr->as.string_lit, out);
        fprintf(out, ", %zu }", expr->as.string_lit.len);
        break;
    case EXPR_BOOL_LIT:
        fprintf(out, "((%s)%d)", c_type(TYPE_BOOL), expr->as.bool_lit ? 1 : 0);
        break;
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (symbol->kind == SYM_CONST && symbol->type != TYPE_STRING) {
            fprintf(out, "((%s)", c_type(symbol->type));
            emit_expr(symbol->decl->as.decl.init, out);
            fputs(")", out);
        } else {
            emit_symbol(symbol, out);
        }
        break;
    }
    case EXPR_UNARY:
        fprintf(out, "((%s)(%s", c_type(expr->type), op_text(expr->as.unary.op));
        emit_expr(expr->as.unary.operand, out);
        fputs("))", out);
        break;
    case EXPR_BINARY:
        emit_binary(expr, out);
        break;
    case EXPR_CALL:
        emit_symbol(expr->as.call.symbol, out);
        fputs("(", out);
        for (size_t i = 0; i < expr->as.call.count; i++) {
            if (i) fputs(", ", out);
            emit_expr(expr->as.call.args[i], out);
        }
        fputs(")", out);
        break;
    case EXPR_CAST:
        fprintf(out, "((%s)", c_type(expr->type));
        emit_expr(expr->as.cast.operand, out);
        fputs(")", out);
        break;
    case EXPR_CIN_OK:
        used.helpers[HELPER_CIN_STATE] = true;
        fputs("wind_cin_ok", out);
        break;
    case EXPR_SIZEOF:
        break; /* rewritten to an integer literal by unit_analyze */
    }
}

static void emit_write(const Expr *arg, FILE *out) {
    static const char *const names[HELPER_COUNT] = {
        [HELPER_WRITE_STRING] = "wind_write_string", [HELPER_WRITE_BOOL] = "wind_write_bool",
        [HELPER_WRITE_CHAR] = "wind_write_char",     [HELPER_WRITE_INT] = "wind_write_int",
        [HELPER_WRITE_UINT] = "wind_write_uint",     [HELPER_WRITE_U128] = "wind_write_u128",
        [HELPER_WRITE_I128] = "wind_write_i128",     [HELPER_WRITE_F16] = "wind_write_f16",
        [HELPER_WRITE_F32] = "wind_write_f32",       [HELPER_WRITE_F64] = "wind_write_f64",
        [HELPER_WRITE_F128] = "wind_write_f128",
    };
    TypeKind type = arg->type;
    Helper helper;
    const char *cast = "";
    switch (type) {
    case TYPE_STRING: helper = HELPER_WRITE_STRING; break;
    case TYPE_BOOL:   helper = HELPER_WRITE_BOOL;   break;
    case TYPE_CHAR:   helper = HELPER_WRITE_CHAR;   break;
    case TYPE_I128:   helper = HELPER_WRITE_I128;   break;
    case TYPE_U128:   helper = HELPER_WRITE_U128;   break;
    case TYPE_F16:    helper = HELPER_WRITE_F16;    break;
    case TYPE_F32:    helper = HELPER_WRITE_F32;    break;
    case TYPE_F128:   helper = HELPER_WRITE_F128;   break;
    case TYPE_FLOAT:
    case TYPE_F64:    helper = HELPER_WRITE_F64;    break;
    default:
        /* Every narrower integer is printed through the widest one. */
        helper = type_is_signed(type) ? HELPER_WRITE_INT : HELPER_WRITE_UINT;
        cast = type_is_signed(type) ? "(long long)" : "(unsigned long long)";
        break;
    }
    used.helpers[helper] = true;
    fprintf(out, "%s(%s", names[helper], cast);
    emit_expr(arg, out);
    fputs(");\n", out);
}

/* The limits passed to the integer readers, as <stdint.h> spells them. */
static const char *int_limits(TypeKind type) {
    switch (type) {
    case TYPE_INT:     return "INTPTR_MIN, INTPTR_MAX";
    case TYPE_I8:      return "INT8_MIN, INT8_MAX";
    case TYPE_I16:     return "INT16_MIN, INT16_MAX";
    case TYPE_I32:     return "INT32_MIN, INT32_MAX";
    case TYPE_I64:     return "INT64_MIN, INT64_MAX";
    case TYPE_U8:      return "UINT8_MAX";
    case TYPE_U16:     return "UINT16_MAX";
    case TYPE_U32:     return "UINT32_MAX";
    case TYPE_U64:     return "UINT64_MAX";
    default:           return "UINTPTR_MAX"; /* uint, uintptr */
    }
}

/* Emits the call that reads one value of the symbol's type from stdin. */
static void emit_read(const Symbol *symbol, FILE *out) {
    TypeKind type = symbol->type;
    Helper helper;
    const char *call;
    switch (type) {
    case TYPE_STRING: helper = HELPER_READ_STRING; call = "wind_read_string()"; break;
    case TYPE_BOOL:   helper = HELPER_READ_BOOL;   call = "wind_read_bool()";   break;
    case TYPE_CHAR:   helper = HELPER_READ_CHAR;   call = "wind_read_char()";   break;
    case TYPE_I128:   helper = HELPER_READ_I128;   call = "wind_read_i128()";   break;
    case TYPE_U128:   helper = HELPER_READ_U128;   call = "wind_read_u128()";   break;
    default:
        if (type_is_float(type)) {
            helper = HELPER_READ_FLOAT;
            call = "wind_read_float()";
        } else {
            helper = type_is_signed(type) ? HELPER_READ_INT : HELPER_READ_UINT;
            call = NULL;
        }
        break;
    }
    used.helpers[helper] = true;
    emit_symbol(symbol, out);
    fprintf(out, " = (%s)", c_type(type));
    if (call) {
        fprintf(out, "%s;\n", call);
    } else {
        fprintf(out, "%s(%s);\n", helper == HELPER_READ_INT ? "wind_read_int" : "wind_read_uint",
                int_limits(type));
    }
}

static void indent(int depth, FILE *out) {
    for (int i = 0; i < depth; i++) fputs("    ", out);
}

static void emit_block(const Block *block, int depth, FILE *out);

static void emit_stmt(const Stmt *stmt, int depth, FILE *out) {
    switch (stmt->kind) {
    case STMT_CONST:
    case STMT_FUNC:
        break;
    case STMT_VAR:
        indent(depth, out);
        emit_symbol(stmt->as.decl.symbol, out);
        fputs(" = ", out);
        if (stmt->as.decl.init) {
            emit_expr(stmt->as.decl.init, out);
        } else {
            fprintf(out, "(%s){0}", c_type(stmt->as.decl.type));
        }
        fputs(";\n", out);
        break;
    case STMT_ASSIGN:
        indent(depth, out);
        emit_symbol(stmt->as.assign.symbol, out);
        fputs(" = ", out);
        if (stmt->as.assign.compound) {
            fprintf(out, "((%s)(", c_type(stmt->as.assign.symbol->type));
            emit_symbol(stmt->as.assign.symbol, out);
            fprintf(out, " %s ", op_text(stmt->as.assign.op));
            emit_expr(stmt->as.assign.value, out);
            fputs("))", out);
        } else {
            emit_expr(stmt->as.assign.value, out);
        }
        fputs(";\n", out);
        break;
    case STMT_EXPR:
        indent(depth, out);
        emit_expr(stmt->as.expr, out);
        fputs(";\n", out);
        break;
    case STMT_COUT:
        for (size_t i = 0; i < stmt->as.cout.count; i++) {
            indent(depth, out);
            emit_write(stmt->as.cout.args[i], out);
        }
        break;
    case STMT_CIN:
        for (size_t i = 0; i < stmt->as.cin.count; i++) {
            indent(depth, out);
            emit_read(stmt->as.cin.targets[i].symbol, out);
        }
        break;
    case STMT_IF:
        indent(depth, out);
        fputs("if (", out);
        emit_expr(stmt->as.if_.cond, out);
        fputs(") {\n", out);
        emit_block(&stmt->as.if_.then_block, depth + 1, out);
        indent(depth, out);
        if (stmt->as.if_.has_else) {
            fputs("} else {\n", out);
            emit_block(&stmt->as.if_.else_block, depth + 1, out);
            indent(depth, out);
        }
        fputs("}\n", out);
        break;
    case STMT_GOTO:
        indent(depth, out);
        fputs("goto ", out);
        emit_symbol(stmt->as.label.symbol, out);
        fputs(";\n", out);
        break;
    case STMT_LABEL:
        /* The empty statement lets a label be the last thing in a block. */
        emit_symbol(stmt->as.label.symbol, out);
        fputs(":;\n", out);
        break;
    case STMT_RETURN:
        indent(depth, out);
        fputs("return", out);
        if (stmt->as.ret.value) {
            fputs(" ", out);
            emit_expr(stmt->as.ret.value, out);
        }
        fputs(";\n", out);
        break;
    }
}

static void emit_block(const Block *block, int depth, FILE *out) {
    for (size_t i = 0; i < block->count; i++) emit_stmt(block->stmts[i], depth, out);
}

static void emit_variables(const SymbolList *variables, const char *prefix, FILE *out) {
    for (size_t i = 0; i < variables->count; i++) {
        fprintf(out, "%s%s ", prefix, c_type(variables->items[i]->type));
        emit_symbol(variables->items[i], out);
        fputs(" = {0};\n", out);
    }
}

static void emit_signature(const Stmt *func, FILE *out) {
    fprintf(out, "static %s ", c_type(func->as.func.ret));
    emit_symbol(func->as.func.symbol, out);
    fputs("(", out);
    if (func->as.func.param_count == 0) fputs("void", out);
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        if (i) fputs(", ", out);
        fprintf(out, "%s ", c_type(func->as.func.params[i].type));
        emit_symbol(func->as.func.params[i].symbol, out);
    }
    fputs(")", out);
}

static void emit_program(const Unit *unit, FILE *out) {
    const Block *top = &unit->program.top;
    bool any = false;

    for (size_t i = 0; i < unit->symbols.count; i++) {
        const Symbol *symbol = unit->symbols.items[i];
        if (symbol->kind != SYM_CONST || symbol->type != TYPE_STRING) continue;
        Str value = const_string(symbol);
        fprintf(out, "static const %s ", c_type(TYPE_STRING));
        emit_symbol(symbol, out);
        fputs(" = { ", out);
        emit_c_string(value, out);
        fprintf(out, ", %zu };\n", value.len);
        any = true;
    }
    emit_variables(&unit->program.globals, "static ", out);
    if (any || unit->program.globals.count) fputs("\n", out);

    /* Prototypes first, so functions can call each other in any order. */
    any = false;
    for (size_t i = 0; i < top->count; i++) {
        if (top->stmts[i]->kind != STMT_FUNC) continue;
        emit_signature(top->stmts[i], out);
        fputs(";\n", out);
        any = true;
    }
    if (any) fputs("\n", out);

    for (size_t i = 0; i < top->count; i++) {
        const Stmt *func = top->stmts[i];
        if (func->kind != STMT_FUNC) continue;
        emit_signature(func, out);
        fputs(" {\n", out);
        emit_variables(&func->as.func.locals, "    ", out);
        emit_block(&func->as.func.body, 1, out);
        fputs("}\n\n", out);
    }

    fputs("int main(void) {\n", out);
    emit_variables(&unit->program.locals, "    ", out);
    emit_block(top, 1, out);
    const Stmt *wind_main = unit->program.main;
    if (wind_main && wind_main->as.func.ret == TYPE_INT) {
        /* The value main returns is the process exit status. */
        fputs("    return (int)", out);
        emit_symbol(wind_main->as.func.symbol, out);
        fputs("();\n}\n", out);
        return;
    }
    if (wind_main) {
        fputs("    ", out);
        emit_symbol(wind_main->as.func.symbol, out);
        fputs("();\n", out);
    }
    fputs("    return 0;\n}\n", out);
}

void emit_c(const Unit *unit, FILE *out) {
    memset(&used, 0, sizeof used);

    char *program = NULL;
    size_t program_len = 0;
    FILE *buffer = open_memstream(&program, &program_len);
    if (!buffer) fatal("out of memory");
    emit_program(unit, buffer);
    if (fclose(buffer) != 0) fatal("out of memory");

    emit_runtime(out);
    fwrite(program, 1, program_len, out);
    free(program);
}
