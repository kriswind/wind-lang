#define _POSIX_C_SOURCE 200809L /* open_memstream */

#include "codegen.h"
#include "targets/target.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Shape of the generated C:
 *
 * - Top-level statements become the body of the C function the target
 *   starts the program with, main() on the machine the compiler runs on,
 *   which then calls the Wind main if there is one. Functions become static
 *   C functions; external ones keep their own names and are not defined.
 * - Variables declared directly at the top level are static objects. Every
 *   other variable is declared at the top of its C function. Either way it
 *   starts zeroed and its Wind declaration becomes an assignment, so a goto
 *   can never jump past an initialization into undefined state.
 * - Non-string constants are substituted at each use. String constants are
 *   static objects.
 * - Arithmetic results are cast back to their Wind type, undoing C's integer
 *   promotion so that narrow types wrap at their own width.
 *
 * - A fixed array is a C array. An array parameter is a slice: a struct of a
 *   pointer and a length, defined once per element type. Every index goes
 *   through wind_index, which counts a negative index back from the end and
 *   stops the program if the index is out of range (a literal index into a
 *   fixed array is settled here and is written as the position), or through
 *   wind_index_unchecked, which leaves that test out, where the build or a
 *   directive asks for it.
 * - A pointer is a C pointer, under a typedef per pointer type. Reaching
 *   through one goes through wind_deref, which stops the program if the
 *   pointer is NULL; moving one is plain C pointer arithmetic.
 * - A "char *" literal is a C string literal, which C ends with a '\0'.
 * - A for loop becomes a C while loop with its last clause at the end of the
 *   body, behind a label that "continue" jumps to. The clauses are whole
 *   statements here, which C's own for header could not hold.
 * - Shifts go through helpers that give every shift amount a defined
 *   result, where C leaves an oversized or negative one undefined.
 * - Only the runtime support a program uses is emitted. The program is
 *   generated first, into memory, recording each runtime type and helper it
 *   mentions; the matching definitions are then written ahead of it.
 *
 * Generated names are prefixed by kind and, for locals, numbered, so no two
 * Wind names can meet in C.
 */

/* ---- Runtime support ---- */

/* The target being compiled for. */
static const Target *target;

/* Whether the code being emitted tests its indexes: what the build asks
   for, unless a directive around the code says otherwise. */
static bool bounds_checked;

/* What the program generated so far needs from the runtime. */
static struct {
    bool types[TYPE_COUNT];
    bool slices[TYPE_COUNT];        /* by element type */
    bool array_writers[TYPE_COUNT]; /* by element type */
    bool helpers[HELPER_COUNT];
    unsigned headers; /* Header flags the program's own code relies on */
    /* The headers named by the external functions and constants the
       program uses, each once. */
    Str *externals;
    size_t external_count;
    size_t external_cap;
} used;

/* The C name of a type, noted as needing its definition. */
static const char *c_type(TypeKind type) {
    /* A declared type or a pointer type is always defined; see
       emit_declared_types. */
    if (type < TYPE_COUNT) used.types[type] = true;
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

/* The helpers that are the same on every target. Those that a target
   supplies are left empty here; see targets/target.h. */
static const HelperInfo helpers[HELPER_COUNT] = {
    [HELPER_HINTS] = {
        "/* Marks a condition that is rarely true and a function that is rarely\n"
        "   called, so that the usual path through the code around them is the\n"
        "   short one. */\n"
        "#if defined(__GNUC__)\n"
        "#define WIND_UNLIKELY(x) __builtin_expect(!!(x), 0)\n"
        "#define WIND_RARE __attribute__((noinline, cold))\n"
        "#else\n"
        "#define WIND_UNLIKELY(x) (x)\n"
        "#define WIND_RARE\n"
        "#endif\n",
        0, {0}, {0}},
    /* An index from 0 up to the length is the usual case and costs one
       comparison. Everything else is handed to a function kept out of line:
       a negative index, which counts back from the end, and an index that
       is out of range. */
    [HELPER_DIGITS] = {
        "/* Writes the decimal digits of v so that they end just before end,\n"
        "   which needs room for twenty of them before it, and returns where\n"
        "   they start. */\n"
        "static char *wind_digits(char *end, unsigned long long v) {\n"
        "    do {\n"
        "        *--end = (char)('0' + (int)(v % 10));\n"
        "        v /= 10;\n"
        "    } while (v);\n"
        "    return end;\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_ERROR_NUMBER] = {
        "static void wind_error_number(long long v, int is_signed) {\n"
        "    char buf[24];\n"
        "    char *end = buf + sizeof buf - 1;\n"
        "    int negative = is_signed && v < 0;\n"
        "    *end = '\\0';\n"
        "    char *start = wind_digits(end, negative ? 0ULL - (unsigned long long)v\n"
        "                                            : (unsigned long long)v);\n"
        "    if (negative) *--start = '-';\n"
        "    wind_error(start);\n"
        "}\n",
        0, {HELPER_DIGITS, HELPER_ERROR}, {0}},
    /* The shift helpers work on the widest type; the caller converts the
       result back, which also discards the bits a narrower type cannot
       hold. An amount that is negative or not less than the width shifts
       every bit out. */
    [HELPER_SHL] = {
        "static unsigned long long wind_shl(unsigned long long v, long long n) {\n"
        "    return (n < 0 || n >= 64) ? 0 : v << n;\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_SHR_UNSIGNED] = {
        "static unsigned long long wind_shr_unsigned(unsigned long long v, long long n) {\n"
        "    return (n < 0 || n >= 64) ? 0 : v >> n;\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_SHR_SIGNED] = {
        "/* Copies of the sign bit are shifted in. */\n"
        "static long long wind_shr_signed(long long v, long long n) {\n"
        "    if (n < 0 || n >= 64) return v < 0 ? -1 : 0;\n"
        "    return v >> n;\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_SHL_128] = {
        "static wind_u128 wind_shl_128(wind_u128 v, long long n) {\n"
        "    return (n < 0 || n >= 128) ? 0 : v << n;\n"
        "}\n",
        0, {0}, {TYPE_U128}},
    [HELPER_SHR_U128] = {
        "static wind_u128 wind_shr_u128(wind_u128 v, long long n) {\n"
        "    return (n < 0 || n >= 128) ? 0 : v >> n;\n"
        "}\n",
        0, {0}, {TYPE_U128}},
    [HELPER_SHR_I128] = {
        "static wind_i128 wind_shr_i128(wind_i128 v, long long n) {\n"
        "    if (n < 0 || n >= 128) return v < 0 ? -1 : 0;\n"
        "    return v >> n;\n"
        "}\n",
        0, {0}, {TYPE_I128}},
    /* An index from 0 up to the length is the usual case and costs one
       comparison. Everything else is handed to a function kept out of line:
       a negative index, which counts back from the end, and an index that
       is out of range. */
    [HELPER_INDEX] = {
        "/* What wind_index does with an index that is not a position as it\n"
        "   stands. A negative index counts back from the end, so -1 is the\n"
        "   last element. An index that is out of range is reported and stops\n"
        "   the program. is_signed tells whether the index had a signed type:\n"
        "   an unsigned one arrives here as negative only when it is too large\n"
        "   to be valid. */\n"
        "WIND_RARE static size_t wind_index_rare(long long i, int is_signed, size_t len, int line) {\n"
        "    long long position = (is_signed && i < 0) ? i + (long long)len : i;\n"
        "    if (position < 0 || (unsigned long long)position >= len) {\n"
        "        wind_flush();\n"
        "        wind_error(\"wind: line \");\n"
        "        wind_error_number(line, 1);\n"
        "        wind_error(\": index \");\n"
        "        wind_error_number(i, is_signed);\n"
        "        wind_error(\" is out of range for length \");\n"
        "        wind_error_number((long long)len, 0);\n"
        "        wind_error(\"\\n\");\n"
        "        wind_stop();\n"
        "    }\n"
        "    return (size_t)position;\n"
        "}\n"
        "\n"
        "/* Turns an index into a position among len elements. */\n"
        "static inline size_t wind_index(long long i, int is_signed, size_t len, int line) {\n"
        "    if (WIND_UNLIKELY((unsigned long long)i >= len)) {\n"
        "        return wind_index_rare(i, is_signed, len, line);\n"
        "    }\n"
        "    return (size_t)i;\n"
        "}\n",
        HEADER_STDDEF,
        {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_INDEX_UNCHECKED] = {
        "WIND_RARE static size_t wind_index_from_end(long long i, size_t len) {\n"
        "    return (size_t)(i + (long long)len);\n"
        "}\n"
        "\n"
        "/* Turns an index into a position among len elements, counting a\n"
        "   negative index back from the end, without testing the position\n"
        "   against len. */\n"
        "static inline size_t wind_index_unchecked(long long i, int is_signed, size_t len) {\n"
        "    if (WIND_UNLIKELY(is_signed && i < 0)) return wind_index_from_end(i, len);\n"
        "    return (size_t)i;\n"
        "}\n",
        HEADER_STDDEF, {HELPER_HINTS}, {0}},
    [HELPER_DEREF] = {
        "/* Returns the pointer it is given, or stops the program if that is\n"
        "   NULL. */\n"
        "static void *wind_deref(void *p, int line) {\n"
        "    if (!p) {\n"
        "        wind_flush();\n"
        "        wind_error(\"wind: line \");\n"
        "        wind_error_number(line, 1);\n"
        "        wind_error(\": a NULL pointer was used\\n\");\n"
        "        wind_stop();\n"
        "    }\n"
        "    return p;\n"
        "}\n",
        0, {HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_STRING_AT] = {
        "static wind_char wind_string_at(wind_string s, long long i, int is_signed, int line) {\n"
        "    return (wind_char)s.data[wind_index(i, is_signed, s.len, line)];\n"
        "}\n",
        0, {HELPER_INDEX}, {TYPE_STRING, TYPE_CHAR}},
    [HELPER_STRING_AT_UNCHECKED] = {
        "static wind_char wind_string_at_unchecked(wind_string s, long long i, int is_signed) {\n"
        "    return (wind_char)s.data[wind_index_unchecked(i, is_signed, s.len)];\n"
        "}\n",
        0, {HELPER_INDEX_UNCHECKED}, {TYPE_STRING, TYPE_CHAR}},
    [HELPER_ARGS] = {
        "/* Wraps the program's C argument vector as an array of strings. */\n"
        "static wind_slice_string wind_args(int argc, char **argv) {\n"
        "    size_t count = argc > 0 ? (size_t)argc : 0;\n"
        "    wind_string *args = malloc(sizeof *args * (count + 1));\n"
        "    if (!args) abort();\n"
        "    for (size_t i = 0; i < count; i++) {\n"
        "        args[i] = (wind_string){ argv[i], strlen(argv[i]) };\n"
        "    }\n"
        "    return (wind_slice_string){ args, count };\n"
        "}\n",
        HEADER_STDLIB | HEADER_STRING, {0}, {TYPE_STRING}},
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
        "/* Reads the next whitespace-delimited word of input into a reused,\n"
        "   NUL-terminated buffer. Returns its length; 0 means the input is\n"
        "   exhausted. Pending output is flushed first, so a prompt is visible. */\n"
        "static size_t wind_read_word(char **word) {\n"
        "    static char *buf;\n"
        "    static size_t cap;\n"
        "    size_t len = 0;\n"
        "    int c;\n"
        "    wind_flush();\n"
        "    do {\n"
        "        c = wind_get();\n"
        "    } while (wind_is_space(c));\n"
        "    while (c != -1 && !wind_is_space(c)) {\n"
        "        if (len + 1 >= cap) {\n"
        "            cap = cap ? cap * 2 : 64;\n"
        "            buf = realloc(buf, cap);\n"
        "            if (!buf) abort();\n"
        "        }\n"
        "        buf[len++] = (char)c;\n"
        "        c = wind_get();\n"
        "    }\n"
        "    if (c != -1) wind_unget(c);\n"
        "    if (buf) buf[len] = '\\0';\n"
        "    *word = buf;\n"
        "    return len;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB,
        {HELPER_IS_SPACE, HELPER_FLUSH, HELPER_GET, HELPER_UNGET}, {0}},
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
        "    wind_flush();\n"
        "    do {\n"
        "        c = wind_get();\n"
        "    } while (wind_is_space(c));\n"
        "    wind_cin_ok = c != -1;\n"
        "    return c == -1 ? 0 : (wind_char)c;\n"
        "}\n",
        0, {HELPER_CIN_STATE, HELPER_IS_SPACE, HELPER_FLUSH, HELPER_GET}, {TYPE_CHAR}},
    [HELPER_READ_INT] = {
        "/* Reads a decimal integer within [min, max], where min is negative. */\n"
        "static long long wind_read_int(long long min, long long max) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    size_t i = 0;\n"
        "    int negative = len && word[0] == '-';\n"
        "    wind_cin_ok = 0;\n"
        "    if (len && (word[0] == '-' || word[0] == '+')) i = 1;\n"
        "    if (i == len) return 0;\n"
        "    unsigned long long limit = negative ? 0ULL - (unsigned long long)min\n"
        "                                        : (unsigned long long)max;\n"
        "    unsigned long long v = 0;\n"
        "    for (; i < len; i++) {\n"
        "        if (word[i] < '0' || word[i] > '9') return 0;\n"
        "        unsigned digit = (unsigned)(word[i] - '0');\n"
        "        if (v > (limit - digit) / 10) return 0;\n"
        "        v = v * 10 + digit;\n"
        "    }\n"
        "    wind_cin_ok = 1;\n"
        "    return negative ? (long long)(0ULL - v) : (long long)v;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDINT, {HELPER_CIN_STATE, HELPER_READ_WORD}, {0}},
    [HELPER_READ_UINT] = {
        "/* Reads a decimal integer within [0, max]. */\n"
        "static unsigned long long wind_read_uint(unsigned long long max) {\n"
        "    char *word;\n"
        "    size_t len = wind_read_word(&word);\n"
        "    size_t i = len && word[0] == '+' ? 1 : 0;\n"
        "    wind_cin_ok = 0;\n"
        "    if (i == len) return 0;\n"
        "    unsigned long long v = 0;\n"
        "    for (; i < len; i++) {\n"
        "        if (word[i] < '0' || word[i] > '9') return 0;\n"
        "        unsigned digit = (unsigned)(word[i] - '0');\n"
        "        if (v > (max - digit) / 10) return 0;\n"
        "        v = v * 10 + digit;\n"
        "    }\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDINT, {HELPER_CIN_STATE, HELPER_READ_WORD}, {0}},
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
        "    long double v = wind_parse_float(word, &end);\n"
        "    if (end != word + len) return 0;\n"
        "    wind_cin_ok = 1;\n"
        "    return v;\n"
        "}\n",
        0, {HELPER_CIN_STATE, HELPER_READ_WORD, HELPER_PARSE_FLOAT}, {0}},
    [HELPER_WRITE_STRING] = {
        "static void wind_write_string(wind_string s) {\n"
        "    wind_put(s.data, s.len);\n"
        "}\n",
        0, {HELPER_PUT}, {TYPE_STRING}},
    [HELPER_STRING_EQ] = {
        "static wind_bool wind_string_eq(wind_string a, wind_string b) {\n"
        "    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n"
        "}\n",
        HEADER_STRING, {0}, {TYPE_STRING, TYPE_BOOL}},
    [HELPER_WRITE_BOOL] = {
        "static void wind_write_bool(wind_bool b) {\n"
        "    if (b) {\n"
        "        wind_put(\"true\", 4);\n"
        "    } else {\n"
        "        wind_put(\"false\", 5);\n"
        "    }\n"
        "}\n",
        0, {HELPER_PUT}, {TYPE_BOOL}},
    [HELPER_WRITE_CHAR] = {
        "static void wind_write_char(wind_char c) {\n"
        "    wind_put((const char *)&c, 1);\n"
        "}\n",
        0, {HELPER_PUT}, {TYPE_CHAR}},
    [HELPER_WRITE_INT] = {
        "static void wind_write_int(long long v) {\n"
        "    char buf[24];\n"
        "    char *end = buf + sizeof buf;\n"
        "    char *start = wind_digits(end, v < 0 ? 0ULL - (unsigned long long)v\n"
        "                                         : (unsigned long long)v);\n"
        "    if (v < 0) *--start = '-';\n"
        "    wind_put(start, (size_t)(end - start));\n"
        "}\n",
        HEADER_STDDEF, {HELPER_DIGITS, HELPER_PUT}, {0}},
    [HELPER_WRITE_UINT] = {
        "static void wind_write_uint(unsigned long long v) {\n"
        "    char buf[24];\n"
        "    char *end = buf + sizeof buf;\n"
        "    char *start = wind_digits(end, v);\n"
        "    wind_put(start, (size_t)(end - start));\n"
        "}\n",
        HEADER_STDDEF, {HELPER_DIGITS, HELPER_PUT}, {0}},
    [HELPER_WRITE_U128] = {
        "static void wind_write_u128(wind_u128 v) {\n"
        "    char buf[40];\n"
        "    char *p = buf + sizeof buf;\n"
        "    do {\n"
        "        *--p = (char)('0' + (int)(v % 10));\n"
        "        v /= 10;\n"
        "    } while (v);\n"
        "    wind_put(p, (size_t)(buf + sizeof buf - p));\n"
        "}\n",
        HEADER_STDDEF, {HELPER_PUT}, {TYPE_U128}},
    [HELPER_WRITE_I128] = {
        "static void wind_write_i128(wind_i128 v) {\n"
        "    if (v < 0) {\n"
        "        wind_put(\"-\", 1);\n"
        "        wind_write_u128(-(wind_u128)v);\n"
        "    } else {\n"
        "        wind_write_u128((wind_u128)v);\n"
        "    }\n"
        "}\n",
        0, {HELPER_WRITE_U128}, {TYPE_I128, TYPE_U128}},
    [HELPER_WRITE_POINTER] = {
        "/* Writes a pointer as its address in hexadecimal, or as NULL. */\n"
        "static void wind_write_pointer(void *p) {\n"
        "    char buf[2 + 2 * sizeof(uintptr_t)];\n"
        "    char *end = buf + sizeof buf;\n"
        "    char *start = end;\n"
        "    uintptr_t v = (uintptr_t)p;\n"
        "    if (!p) {\n"
        "        wind_put(\"NULL\", 4);\n"
        "        return;\n"
        "    }\n"
        "    do {\n"
        "        *--start = \"0123456789abcdef\"[v % 16];\n"
        "        v /= 16;\n"
        "    } while (v);\n"
        "    *--start = 'x';\n"
        "    *--start = '0';\n"
        "    wind_put(start, (size_t)(end - start));\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDINT, {HELPER_PUT}, {0}},
    [HELPER_WRITE_CSTRING] = {
        "/* Writes the characters up to the '\\0' that ends them. */\n"
        "static void wind_write_cstring(const wind_char *s) {\n"
        "    if (s) {\n"
        "        wind_put((const char *)s, strlen((const char *)s));\n"
        "    } else {\n"
        "        wind_put(\"NULL\", 4);\n"
        "    }\n"
        "}\n",
        HEADER_STRING, {HELPER_PUT}, {TYPE_CHAR}},
    [HELPER_STRING_FROM] = {
        "/* The characters up to the '\\0' that ends them, as a string that\n"
        "   views them where they are. */\n"
        "static wind_string wind_string_from(const wind_char *s) {\n"
        "    wind_string view = { (const char *)s, s ? strlen((const char *)s) : 0 };\n"
        "    return view;\n"
        "}\n",
        HEADER_STRING, {0}, {TYPE_STRING, TYPE_CHAR}},
    [HELPER_CSTRING] = {
        "/* A copy of the string with a '\\0' after it, in memory that is never\n"
        "   freed. */\n"
        "static wind_char *wind_cstring(wind_string s) {\n"
        "    wind_char *copy = malloc(s.len + 1);\n"
        "    if (!copy) abort();\n"
        "    if (s.len) memcpy(copy, s.data, s.len);\n"
        "    copy[s.len] = 0;\n"
        "    return copy;\n"
        "}\n",
        HEADER_STDLIB | HEADER_STRING, {0}, {TYPE_STRING, TYPE_CHAR}},
    [HELPER_WRITE_ESCAPED] = {
        "/* Writes one byte as it is spelled inside a literal that uses the\n"
        "   given quote character. */\n"
        "static void wind_write_escaped(unsigned char c, char quote) {\n"
        "    switch (c) {\n"
        "    case '\\n': wind_put(\"\\\\n\", 2); break;\n"
        "    case '\\t': wind_put(\"\\\\t\", 2); break;\n"
        "    case '\\r': wind_put(\"\\\\r\", 2); break;\n"
        "    case '\\0': wind_put(\"\\\\0\", 2); break;\n"
        "    case '\\\\': wind_put(\"\\\\\\\\\", 2); break;\n"
        "    default:\n"
        "        if (c == (unsigned char)quote) wind_put(\"\\\\\", 1);\n"
        "        wind_put((const char *)&c, 1);\n"
        "    }\n"
        "}\n",
        0, {HELPER_PUT}, {0}},
    [HELPER_WRITE_QUOTED_STRING] = {
        "static void wind_write_quoted_string(wind_string s) {\n"
        "    wind_put(\"\\\"\", 1);\n"
        "    for (size_t i = 0; i < s.len; i++) wind_write_escaped((unsigned char)s.data[i], '\"');\n"
        "    wind_put(\"\\\"\", 1);\n"
        "}\n",
        HEADER_STDDEF, {HELPER_WRITE_ESCAPED, HELPER_PUT}, {TYPE_STRING}},
    [HELPER_WRITE_QUOTED_CSTRING] = {
        "static void wind_write_quoted_cstring(const wind_char *s) {\n"
        "    if (!s) {\n"
        "        wind_put(\"NULL\", 4);\n"
        "        return;\n"
        "    }\n"
        "    wind_put(\"\\\"\", 1);\n"
        "    for (; *s; s++) wind_write_escaped(*s, '\"');\n"
        "    wind_put(\"\\\"\", 1);\n"
        "}\n",
        0, {HELPER_WRITE_ESCAPED, HELPER_PUT}, {TYPE_CHAR}},
    [HELPER_WRITE_QUOTED_CHAR] = {
        "static void wind_write_quoted_char(wind_char c) {\n"
        "    wind_put(\"'\", 1);\n"
        "    wind_write_escaped(c, '\\'');\n"
        "    wind_put(\"'\", 1);\n"
        "}\n",
        0, {HELPER_WRITE_ESCAPED, HELPER_PUT}, {TYPE_CHAR}},
};

/* What a helper's definition is: the target's own for the helpers a target
   supplies, the shared one for the rest. */
static const HelperInfo *helper_info(int helper) {
    if (helper >= HELPER_TARGET_FIRST && helper <= HELPER_TARGET_LAST) {
        return &target->helpers[helper];
    }
    return &helpers[helper];
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

/* Notes that the program uses an external function or constant, whose
   header must then be included. */
static void use_external(const Stmt *decl) {
    Str header = decl->header;
    if (!header.len) return;
    for (size_t i = 0; i < used.external_count; i++) {
        if (str_eq(used.externals[i], header)) return;
    }
    if (used.external_count == used.external_cap) {
        used.external_cap = used.external_cap ? used.external_cap * 2 : 4;
        used.externals = xrealloc(used.externals, used.external_cap * sizeof *used.externals);
    }
    used.externals[used.external_count++] = header;
}

static void emit_symbol(const Symbol *symbol, FILE *out) {
    int len = (int)symbol->name.len;
    const char *name = symbol->name.data;
    if (symbol->decl && symbol->decl->external) {
        use_external(symbol->decl);
        fprintf(out, "%.*s", len, name);
        return;
    }
    /* What an imported file declares carries the file's number, so that two
       files can use one name. */
    fputs("wind_", out);
    if (symbol->unit->id) fprintf(out, "m%d_", symbol->unit->id);
    switch (symbol->kind) {
    case SYM_FUNC:   fprintf(out, "f_%.*s", len, name); break;
    case SYM_LABEL:  fprintf(out, "l_%.*s", len, name); break;
    case SYM_CONST:  fprintf(out, "c%d_%.*s", symbol->id, len, name); break;
    case SYM_VAR:
    case SYM_PARAM:  fprintf(out, "v%d_%.*s", symbol->id, len, name); break;
    case SYM_MODULE:
    case SYM_TYPE:
    case SYM_ENUM_VALUE:
        break; /* never emitted by name: none of these is stored */
    }
}

/* The string literal a constant string expression ultimately names,
   following constant-to-constant initializers. */
static Str const_string(const Expr *expr) {
    while (expr->kind == EXPR_IDENT) expr = expr->as.ident.symbol->decl->as.decl.init;
    return expr->as.string_lit;
}

/* The slice type for an element type, noted as needing its definition. */
static void emit_slice_type(TypeKind elem, FILE *out) {
    TypeInfo *info = type_info(elem);
    if (info) {
        info->slice_used = true;
        fprintf(out, "wind_slice_%s", info->c_stem);
        return;
    }
    used.slices[elem] = true;
    fprintf(out, "wind_slice_%s", type_name(elem));
}

/* True for a constant that is stored in the generated program, and so is
   referred to by name; other constants are written out where they are used. */
static bool is_stored_const(const Symbol *symbol) {
    return symbol->kind == SYM_CONST &&
           (symbol->is_array || symbol->type == TYPE_STRING || type_is_struct(symbol->type));
}

static void emit_field_name(Str name, FILE *out) {
    fprintf(out, "f_%.*s", (int)name.len, name.data);
}

/* Emits the place that a read stores into. */
static void emit_place(const Symbol *symbol, const Expr *index, const PlaceField *fields,
                       size_t field_count, FILE *out);

/* True for an array's name that stands for the array itself, and not for a
   pointer to its first element. */
static bool is_array_name(const Expr *expr) {
    return expr->kind == EXPR_IDENT && expr->as.ident.symbol && !expr->as.ident.decayed &&
           (expr->as.ident.symbol->is_array || expr->as.ident.symbol->is_slice);
}

/* Emits a pointer, checked for NULL, as a pointer of its own type again. */
static void emit_checked_pointer(const Expr *pointer, FILE *out) {
    used.helpers[HELPER_DEREF] = true;
    fprintf(out, "((%s)wind_deref(", c_type(pointer->type));
    emit_expr(pointer, out);
    fprintf(out, ", %d))", pointer->loc.line);
}

/* If the index is a literal, possibly negated, that is within a fixed
   array's length, sets *position to the element it names and returns true.
   Such an index needs nothing worked out when the program runs. */
static bool literal_position(const Symbol *array, const Expr *index, size_t *position) {
    if (array->is_slice) return false;
    bool negated = index->kind == EXPR_UNARY && index->as.unary.op == OP_NEG;
    const Expr *literal = negated ? index->as.unary.operand : index;
    if (literal->kind != EXPR_INT_LIT) return false;
    U128 value = literal->as.int_lit;
    if (negated && value != 0) {
        if (value > array->length) return false;
        *position = array->length - (size_t)value;
        return true;
    }
    if (value >= array->length) return false;
    *position = (size_t)value;
    return true;
}

/* Emits one element of an array as something that can be read or assigned. */
static void emit_element(const Symbol *array, const Expr *index, FILE *out) {
    size_t position;
    if (literal_position(array, index, &position)) {
        emit_symbol(array, out);
        fprintf(out, "[%zu]", position);
        return;
    }
    used.helpers[bounds_checked ? HELPER_INDEX : HELPER_INDEX_UNCHECKED] = true;
    emit_symbol(array, out);
    fprintf(out, "%s[%s((long long)(", array->is_slice ? ".data" : "",
            bounds_checked ? "wind_index" : "wind_index_unchecked");
    emit_expr(index, out);
    fprintf(out, "), %d, ", type_is_signed(index->type));
    if (array->is_slice) {
        emit_symbol(array, out);
        fputs(".len", out);
    } else {
        fprintf(out, "%zu", array->length);
    }
    if (bounds_checked) fprintf(out, ", %d", index->loc.line);
    fputs(")]", out);
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

/* Emits the start of a shift of a value of the given type: everything up to
   the value itself. emit_shift_end completes it. */
static void emit_shift_start(Op op, TypeKind type, FILE *out) {
    bool wide = type == TYPE_I128 || type == TYPE_U128;
    Helper helper;
    const char *call;
    if (op == OP_SHL) {
        helper = wide ? HELPER_SHL_128 : HELPER_SHL;
        call = wide ? "wind_shl_128((wind_u128)(" : "wind_shl((unsigned long long)(";
    } else if (type_is_signed(type)) {
        helper = wide ? HELPER_SHR_I128 : HELPER_SHR_SIGNED;
        call = wide ? "wind_shr_i128((wind_i128)(" : "wind_shr_signed((long long)(";
    } else {
        helper = wide ? HELPER_SHR_U128 : HELPER_SHR_UNSIGNED;
        call = wide ? "wind_shr_u128((wind_u128)(" : "wind_shr_unsigned((unsigned long long)(";
    }
    used.helpers[helper] = true;
    fprintf(out, "((%s)%s", c_type(type), call);
}

static void emit_shift_end(const Expr *amount, FILE *out) {
    fputs("), (long long)(", out);
    emit_expr(amount, out);
    fputs(")))", out);
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
    if (op_is_shift(op)) {
        emit_shift_start(op, expr->type, out);
        emit_expr(left, out);
        emit_shift_end(right, out);
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
    case EXPR_INDEX: {
        const Expr *base = expr->as.index.base;
        if (is_array_name(base)) {
            emit_element(base->as.ident.symbol, expr->as.index.index, out);
            break;
        }
        if (type_is_pointer(base->type)) {
            emit_checked_pointer(base, out);
            fputs("[", out);
            emit_expr(expr->as.index.index, out);
            fputs("]", out);
            break;
        }
        used.helpers[bounds_checked ? HELPER_STRING_AT : HELPER_STRING_AT_UNCHECKED] = true;
        fputs(bounds_checked ? "wind_string_at(" : "wind_string_at_unchecked(", out);
        emit_expr(base, out);
        fputs(", (long long)(", out);
        emit_expr(expr->as.index.index, out);
        fprintf(out, "), %d", type_is_signed(expr->as.index.index->type));
        if (bounds_checked) fprintf(out, ", %d", expr->loc.line);
        fputs(")", out);
        break;
    }
    case EXPR_LEN: {
        const Expr *operand = expr->as.len.operand;
        fprintf(out, "((%s)", c_type(TYPE_INT));
        bool is_const_string = operand->kind == EXPR_STRING_LIT ||
                               (operand->kind == EXPR_IDENT && !is_array_name(operand) &&
                                operand->as.ident.symbol->kind == SYM_CONST);
        if (is_const_string) {
            /* Written as a number so that it can sit in a C initializer. */
            fprintf(out, "%zu)", const_string(operand).len);
        } else if (!is_array_name(operand)) {
            fputs("(", out);
            emit_expr(operand, out);
            fputs(").len)", out);
        } else if (operand->as.ident.symbol->is_slice) {
            emit_symbol(operand->as.ident.symbol, out);
            fputs(".len)", out);
        } else {
            fprintf(out, "%zu)", operand->as.ident.symbol->length);
        }
        break;
    }
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
        if (expr->type != TYPE_STRING) {
            fprintf(out, "((%s)", c_type(expr->type));
            emit_c_string(expr->as.string_lit, out);
            fputs(")", out);
            break;
        }
        fprintf(out, "(%s){ ", c_type(TYPE_STRING));
        emit_c_string(expr->as.string_lit, out);
        fprintf(out, ", %zu }", expr->as.string_lit.len);
        break;
    case EXPR_BOOL_LIT:
        fprintf(out, "((%s)%d)", c_type(TYPE_BOOL), expr->as.bool_lit ? 1 : 0);
        break;
    case EXPR_NULL:
        fprintf(out, "((%s)0)", c_type(expr->type));
        break;
    case EXPR_FIELD:
        if (expr->as.field.through_pointer) {
            emit_checked_pointer(expr->as.field.base, out);
            fputs("->", out);
        } else {
            fputs("(", out);
            emit_expr(expr->as.field.base, out);
            fputs(").", out);
        }
        emit_field_name(expr->as.field.name, out);
        break;
    case EXPR_INIT: {
        const TypeInfo *info = type_info(expr->type);
        fprintf(out, "((%s){ ", c_type(expr->type));
        for (size_t i = 0; i < expr->as.init.count; i++) {
            const InitItem *item = &expr->as.init.items[i];
            if (i) fputs(", ", out);
            fputs(".", out);
            emit_field_name(info->fields[item->index].name, out);
            fputs(" = ", out);
            emit_expr(item->value, out);
        }
        /* Fields left out are zero, as in C. */
        if (expr->as.init.count == 0) fputs("0", out);
        fputs(" })", out);
        break;
    }
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (symbol->kind == SYM_ENUM_VALUE) {
            fprintf(out, "((%s)%lld)", c_type(symbol->type), symbol->value);
        } else if (symbol->kind == SYM_CONST && symbol->decl->external) {
            fprintf(out, "((%s)", c_type(symbol->type));
            emit_symbol(symbol, out);
            fputs(")", out);
        } else if (symbol->kind == SYM_CONST && !is_stored_const(symbol)) {
            fprintf(out, "((%s)", c_type(symbol->type));
            emit_expr(symbol->decl->as.decl.init, out);
            fputs(")", out);
        } else if (expr->as.ident.decayed && symbol->is_slice) {
            emit_symbol(symbol, out);
            fputs(".data", out);
        } else {
            emit_symbol(symbol, out);
        }
        break;
    }
    case EXPR_UNARY:
        if (expr->as.unary.op == OP_ADDR) {
            fputs("(&", out);
            emit_expr(expr->as.unary.operand, out);
            fputs(")", out);
            break;
        }
        if (expr->as.unary.op == OP_DEREF) {
            fputs("(*", out);
            emit_checked_pointer(expr->as.unary.operand, out);
            fputs(")", out);
            break;
        }
        fprintf(out, "((%s)(%s", c_type(expr->type), op_text(expr->as.unary.op));
        emit_expr(expr->as.unary.operand, out);
        fputs("))", out);
        break;
    case EXPR_BINARY:
        emit_binary(expr, out);
        break;
    case EXPR_CALL: {
        /* Pointers cross into and out of C through void *, since C's own
           declaration may differ in what it points to (char against the
           unsigned char of Wind, or const), and Wind has already checked
           them against the extern declaration. */
        bool external = expr->as.call.symbol->decl->external;
        bool cast_result = external && type_is_pointer(expr->type);
        if (cast_result) fprintf(out, "((%s)(void *)", c_type(expr->type));
        emit_symbol(expr->as.call.symbol, out);
        fputs("(", out);
        for (size_t i = 0; i < expr->as.call.count; i++) {
            const Expr *arg = expr->as.call.args[i];
            if (i) fputs(", ", out);
            if (external && type_is_pointer(arg->type)) {
                fputs("(void *)", out);
                emit_expr(arg, out);
            } else if (!is_array_name(arg)) {
                emit_expr(arg, out);
            } else if (arg->as.ident.symbol->is_slice) {
                emit_symbol(arg->as.ident.symbol, out);
            } else {
                /* A fixed array is passed as a view of all its elements. */
                fputs("(", out);
                emit_slice_type(arg->as.ident.symbol->type, out);
                fputs("){ ", out);
                emit_symbol(arg->as.ident.symbol, out);
                fprintf(out, ", %zu }", arg->as.ident.symbol->length);
            }
        }
        fputs(cast_result ? "))" : ")", out);
        break;
    }
    case EXPR_CAST:
        if (type_is_struct(expr->type)) {
            /* Only a struct converts to a struct, and C has no cast for it. */
            emit_expr(expr->as.cast.operand, out);
            break;
        }
        if (expr->type != expr->as.cast.operand->type &&
            (expr->type == TYPE_STRING || expr->as.cast.operand->type == TYPE_STRING)) {
            /* Between a string and a "char *", in one direction or the
               other. */
            bool to_string = expr->type == TYPE_STRING;
            used.helpers[to_string ? HELPER_STRING_FROM : HELPER_CSTRING] = true;
            fputs(to_string ? "wind_string_from(" : "wind_cstring(", out);
            emit_expr(expr->as.cast.operand, out);
            fputs(")", out);
            break;
        }
        fprintf(out, "((%s)", c_type(expr->type));
        emit_expr(expr->as.cast.operand, out);
        fputs(")", out);
        break;
    case EXPR_TERNARY:
        fputs("((", out);
        emit_expr(expr->as.ternary.cond, out);
        fputs(") ? (", out);
        emit_expr(expr->as.ternary.then, out);
        fputs(") : (", out);
        emit_expr(expr->as.ternary.otherwise, out);
        fputs("))", out);
        break;
    case EXPR_CIN_OK:
        used.helpers[HELPER_CIN_STATE] = true;
        fputs("wind_cin_ok", out);
        break;
    case EXPR_SIZEOF:
        break; /* rewritten to an integer literal by unit_analyze */
    }
}

/* The helper that prints one value of a type, noted as used. *cast is what
   the value must be converted to first, or "". Inside an array, strings and
   chars are quoted so that the elements can be told apart. */
static const char *value_writer(TypeKind type, bool in_array, const char **cast) {
    static const char *const names[HELPER_COUNT] = {
        [HELPER_WRITE_STRING] = "wind_write_string",
        [HELPER_WRITE_BOOL] = "wind_write_bool",
        [HELPER_WRITE_CHAR] = "wind_write_char",
        [HELPER_WRITE_INT] = "wind_write_int",
        [HELPER_WRITE_UINT] = "wind_write_uint",
        [HELPER_WRITE_U128] = "wind_write_u128",
        [HELPER_WRITE_I128] = "wind_write_i128",
        [HELPER_WRITE_F16] = "wind_write_f16",
        [HELPER_WRITE_F32] = "wind_write_f32",
        [HELPER_WRITE_F64] = "wind_write_f64",
        [HELPER_WRITE_F128] = "wind_write_f128",
        [HELPER_WRITE_POINTER] = "wind_write_pointer",
        [HELPER_WRITE_CSTRING] = "wind_write_cstring",
        [HELPER_WRITE_QUOTED_CSTRING] = "wind_write_quoted_cstring",
        [HELPER_WRITE_QUOTED_STRING] = "wind_write_quoted_string",
        [HELPER_WRITE_QUOTED_CHAR] = "wind_write_quoted_char",
    };
    Helper helper;
    *cast = "";
    TypeInfo *info = type_info(type);
    if (info && info->is_pointer && info->target == TYPE_CHAR) {
        /* A "char *" prints as the text it points to. */
        helper = in_array ? HELPER_WRITE_QUOTED_CSTRING : HELPER_WRITE_CSTRING;
        used.helpers[helper] = true;
        return names[helper];
    }
    if (info && info->is_pointer) {
        used.helpers[HELPER_WRITE_POINTER] = true;
        *cast = "(void *)";
        return names[HELPER_WRITE_POINTER];
    }
    if (info) {
        info->writer_used = true;
        return info->writer_name;
    }
    switch (type) {
    case TYPE_STRING: helper = in_array ? HELPER_WRITE_QUOTED_STRING : HELPER_WRITE_STRING; break;
    case TYPE_CHAR:   helper = in_array ? HELPER_WRITE_QUOTED_CHAR : HELPER_WRITE_CHAR;     break;
    case TYPE_BOOL:   helper = HELPER_WRITE_BOOL; break;
    case TYPE_I128:   helper = HELPER_WRITE_I128; break;
    case TYPE_U128:   helper = HELPER_WRITE_U128; break;
    case TYPE_F16:    helper = HELPER_WRITE_F16;  break;
    case TYPE_F32:    helper = HELPER_WRITE_F32;  break;
    case TYPE_F128:   helper = HELPER_WRITE_F128; break;
    case TYPE_FLOAT:
    case TYPE_F64:    helper = HELPER_WRITE_F64;  break;
    default:
        /* Every narrower integer is printed through the widest one. */
        helper = type_is_signed(type) ? HELPER_WRITE_INT : HELPER_WRITE_UINT;
        *cast = type_is_signed(type) ? "(long long)" : "(unsigned long long)";
        break;
    }
    used.helpers[helper] = true;
    return names[helper];
}

static void emit_write(const Expr *arg, FILE *out) {
    if (is_array_name(arg)) {
        /* A whole array prints as "[a, b, c]". */
        const Symbol *array = arg->as.ident.symbol;
        TypeInfo *info = type_info(array->type);
        if (info) {
            info->array_writer_used = true;
            fprintf(out, "wind_write_array_%s(", info->c_stem);
        } else {
            used.array_writers[array->type] = true;
            fprintf(out, "wind_write_array_%s(", type_name(array->type));
        }
        emit_symbol(array, out);
        if (array->is_slice) {
            fputs(".data, ", out);
            emit_symbol(array, out);
            fputs(".len);\n", out);
        } else {
            fprintf(out, ", %zu);\n", array->length);
        }
        return;
    }
    const char *cast;
    const char *writer = value_writer(arg->type, false, &cast);
    fprintf(out, "%s(%s", writer, cast);
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

/* Emits the statement that reads one value from stdin into the target. */
static void emit_read(const ReadTarget *target, FILE *out) {
    const Symbol *symbol = target->symbol;
    TypeKind type = target->type;
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
    emit_place(symbol, target->index, target->fields, target->field_count, out);
    /* A string is read as one already; every other value is converted
       from the widest type of its kind. */
    if (type == TYPE_STRING) {
        fputs(" = ", out);
    } else {
        fprintf(out, " = (%s)", c_type(type));
    }
    if (call) {
        fprintf(out, "%s;\n", call);
    } else {
        fprintf(out, "%s(%s);\n", helper == HELPER_READ_INT ? "wind_read_int" : "wind_read_uint",
                int_limits(type));
    }
}

static void emit_place(const Symbol *symbol, const Expr *index, const PlaceField *fields,
                       size_t field_count, FILE *out) {
    if (index) {
        emit_element(symbol, index, out);
    } else {
        emit_symbol(symbol, out);
    }
    for (size_t i = 0; i < field_count; i++) {
        fputs(".", out);
        emit_field_name(fields[i].name, out);
    }
}

/* True for an assignment target that is a variable or a field of one, which
   costs nothing to write out twice. */
static bool is_plain_place(const Expr *expr) {
    while (expr->kind == EXPR_FIELD && !expr->as.field.through_pointer) {
        expr = expr->as.field.base;
    }
    return expr->kind == EXPR_IDENT;
}

static void indent(int depth, FILE *out) {
    for (int i = 0; i < depth; i++) fputs("    ", out);
}

static void emit_block(const Block *block, int depth, FILE *out);

/* True if a file's top-level code can run a statement more than once: it
   has a label there, which a goto can jump back to. */
static bool top_level_repeats(const Unit *unit) {
    for (size_t i = 0; i < unit->symbols.count; i++) {
        const Symbol *symbol = unit->symbols.items[i];
        if (symbol->kind == SYM_LABEL && !symbol->func) return true;
    }
    return false;
}

static void emit_stmt_kind(const Stmt *stmt, int depth, FILE *out);

/* Emits a statement under the bounds checking its directive asks for. */
static void emit_stmt(const Stmt *stmt, int depth, FILE *out) {
    bool outer = bounds_checked;
    if (stmt->bounds != BOUNDS_INHERIT) bounds_checked = stmt->bounds == BOUNDS_CHECKED;
    emit_stmt_kind(stmt, depth, out);
    bounds_checked = outer;
}

static void emit_stmt_kind(const Stmt *stmt, int depth, FILE *out) {
    switch (stmt->kind) {
    case STMT_BLOCK:
        indent(depth, out);
        fputs("{\n", out);
        emit_block(&stmt->as.block, depth + 1, out);
        indent(depth, out);
        fputs("}\n", out);
        break;
    case STMT_CONST:
    case STMT_FUNC:
    case STMT_IMPORT:
    case STMT_TYPE:
    case STMT_EXTERN_GROUP:
    case STMT_SETTING:
        break;
    case STMT_VAR:
        indent(depth, out);
        if (stmt->as.decl.is_array) {
            /* Elements the initializer leaves out are zero, as in C. */
            const Symbol *array = stmt->as.decl.symbol;
            if (stmt->as.decl.elem_count == 0 && array->global && !top_level_repeats(array->unit)) {
                /* Its storage starts out zero and this runs once, so there
                   is nothing to clear. */
                fputs(";\n", out);
                break;
            }
            used.headers |= HEADER_STRING;
            if (stmt->as.decl.elem_count == 0) {
                fputs("memset(", out);
                emit_symbol(array, out);
                fputs(", 0, sizeof ", out);
            } else {
                fputs("memcpy(", out);
                emit_symbol(array, out);
                fprintf(out, ", (%s[%zu]){ ", c_type(array->type), array->length);
                for (size_t i = 0; i < stmt->as.decl.elem_count; i++) {
                    if (i) fputs(", ", out);
                    emit_expr(stmt->as.decl.elems[i], out);
                }
                fputs(" }, sizeof ", out);
            }
            emit_symbol(array, out);
            fputs(");\n", out);
            break;
        }
        emit_symbol(stmt->as.decl.symbol, out);
        fputs(" = ", out);
        if (stmt->as.decl.init) {
            emit_expr(stmt->as.decl.init, out);
        } else {
            fprintf(out, "(%s){0}", c_type(stmt->as.decl.type));
        }
        fputs(";\n", out);
        break;
    case STMT_ASSIGN: {
        const Expr *target = stmt->as.assign.target;
        const Expr *value = stmt->as.assign.value;
        TypeKind type = target->type;
        Op op = stmt->as.assign.op;
        bool compound = stmt->as.assign.compound;
        /* A target with an index or a pointer in it is written out once, so
           that what it computes is computed once. */
        bool once = !is_plain_place(target);
        indent(depth, out);
        if (compound && op_is_shift(op) && once) {
            fprintf(out, "{ %s *place = &", c_type(type));
            emit_expr(target, out);
            fputs("; *place = ", out);
            emit_shift_start(op, type, out);
            fputs("*place", out);
            emit_shift_end(value, out);
            fputs("; }\n", out);
            break;
        }
        emit_expr(target, out);
        if (compound && op_is_shift(op)) {
            fputs(" = ", out);
            emit_shift_start(op, type, out);
            emit_expr(target, out);
            emit_shift_end(value, out);
        } else if (compound && once) {
            /* C's own compound operator wraps at the place's width as well. */
            fprintf(out, " %s= ", op_text(op));
            emit_expr(value, out);
        } else if (compound) {
            fprintf(out, " = ((%s)(", c_type(type));
            emit_expr(target, out);
            fprintf(out, " %s ", op_text(op));
            emit_expr(value, out);
            fputs("))", out);
        } else {
            fputs(" = ", out);
            emit_expr(value, out);
        }
        fputs(";\n", out);
        break;
    }
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
            emit_read(&stmt->as.cin.targets[i], out);
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
    case STMT_WHILE:
    case STMT_FOR:
        if (stmt->as.loop.init) emit_stmt(stmt->as.loop.init, depth, out);
        indent(depth, out);
        fputs("while (", out);
        if (stmt->as.loop.cond) {
            emit_expr(stmt->as.loop.cond, out);
        } else {
            fputs("1", out);
        }
        fputs(") {\n", out);
        emit_block(&stmt->as.loop.body, depth + 1, out);
        if (stmt->as.loop.step) {
            fprintf(out, "wind_step_%d:;\n", stmt->as.loop.id);
            emit_stmt(stmt->as.loop.step, depth + 1, out);
        }
        indent(depth, out);
        fputs("}\n", out);
        break;
    case STMT_DO_WHILE:
        indent(depth, out);
        fputs("do {\n", out);
        emit_block(&stmt->as.loop.body, depth + 1, out);
        indent(depth, out);
        fputs("} while (", out);
        emit_expr(stmt->as.loop.cond, out);
        fputs(");\n", out);
        break;
    case STMT_BREAK:
        indent(depth, out);
        fputs("break;\n", out);
        break;
    case STMT_CONTINUE:
        indent(depth, out);
        if (stmt->as.target->as.loop.step) {
            fprintf(out, "goto wind_step_%d;\n", stmt->as.target->as.loop.id);
        } else {
            fputs("continue;\n", out);
        }
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
        const Symbol *variable = variables->items[i];
        fprintf(out, "%s%s ", prefix, c_type(variable->type));
        emit_symbol(variable, out);
        if (variable->is_array) fprintf(out, "[%zu]", variable->length);
        fputs(" = {0};\n", out);
    }
}

static void emit_signature(const Stmt *func, FILE *out) {
    fprintf(out, "static %s ", c_type(func->as.func.ret));
    emit_symbol(func->as.func.symbol, out);
    fputs("(", out);
    if (func->as.func.param_count == 0) fputs("void", out);
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        const Param *param = &func->as.func.params[i];
        if (i) fputs(", ", out);
        if (param->is_slice) {
            emit_slice_type(param->type, out);
            fputs(" ", out);
        } else {
            fprintf(out, "%s ", c_type(param->type));
        }
        emit_symbol(param->symbol, out);
    }
    fputs(")", out);
}

/* Writes the headers, type definitions and helpers recorded in `used`. */
static void emit_runtime(FILE *out) {
    /* Each array writer prints its elements with the writer for one value. */
    const char *element_writers[TYPE_COUNT] = {0};
    const char *element_casts[TYPE_COUNT] = {0};
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.array_writers[type]) continue;
        element_writers[type] = value_writer(type, true, &element_casts[type]);
        used.types[type] = true;
        used.headers |= HEADER_STDDEF;
        used.helpers[HELPER_PUT] = true;
    }

    /* A helper can need another that sits anywhere in the list, so the set
       is grown until it stops changing. */
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < HELPER_COUNT; i++) {
            if (!used.helpers[i]) continue;
            const HelperInfo *info = helper_info(i);
            for (size_t j = 0; j < 4 && info->needs[j] != HELPER_NONE; j++) {
                if (!used.helpers[info->needs[j]]) {
                    used.helpers[info->needs[j]] = true;
                    changed = true;
                }
            }
        }
    }

    unsigned headers = used.headers;
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.slices[type]) continue;
        used.types[type] = true;
        headers |= HEADER_STDDEF;
    }
    for (int i = 0; i < HELPER_COUNT; i++) {
        if (!used.helpers[i]) continue;
        const HelperInfo *info = helper_info(i);
        headers |= info->headers;
        for (size_t j = 0; j < 3 && info->types[j] != TYPE_INVALID; j++) {
            used.types[info->types[j]] = true;
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
    for (size_t i = 0; i < used.external_count; i++) {
        fprintf(out, "#include \"%.*s\"\n", (int)used.externals[i].len,
                used.externals[i].data);
    }
    if (headers || used.external_count) fputs("\n", out);

    bool any = false;
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.types[type] || !type_definitions[type]) continue;
        fputs(type_definitions[type], out);
        any = true;
    }
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.slices[type]) continue;
        fprintf(out, "typedef struct {\n    %s *data;\n    size_t len;\n} wind_slice_%s;\n",
                type_c_name(type), type_name(type));
        any = true;
    }
    if (any) fputs("\n", out);

    for (int i = 0; i < HELPER_COUNT; i++) {
        if (used.helpers[i]) fprintf(out, "%s\n", helper_info(i)->definition);
    }
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.array_writers[type]) continue;
        fprintf(out,
                "static void wind_write_array_%s(const %s *data, size_t len) {\n"
                "    wind_put(\"[\", 1);\n"
                "    for (size_t i = 0; i < len; i++) {\n"
                "        if (i) wind_put(\", \", 2);\n"
                "        %s(%sdata[i]);\n"
                "    }\n"
                "    wind_put(\"]\", 1);\n"
                "}\n\n",
                type_name(type), type_c_name(type), element_writers[type],
                element_casts[type]);
    }
}

/* Emits a constant value in the form C accepts for an object that lives for
   the whole program: braces instead of the compound literals that emit_expr
   writes, and the value of another stored constant instead of its name. */
static void emit_static_value(const Expr *expr, FILE *out) {
    while (expr->kind == EXPR_IDENT && is_stored_const(expr->as.ident.symbol)) {
        expr = expr->as.ident.symbol->decl->as.decl.init;
    }
    if (expr->kind == EXPR_CAST && type_is_struct(expr->type)) {
        emit_static_value(expr->as.cast.operand, out);
    } else if (expr->kind == EXPR_STRING_LIT && expr->type == TYPE_STRING) {
        fputs("{ ", out);
        emit_c_string(expr->as.string_lit, out);
        fprintf(out, ", %zu }", expr->as.string_lit.len);
    } else if (expr->kind == EXPR_INIT) {
        const TypeInfo *info = type_info(expr->type);
        fputs("{ ", out);
        for (size_t i = 0; i < expr->as.init.count; i++) {
            const InitItem *item = &expr->as.init.items[i];
            if (i) fputs(", ", out);
            fputs(".", out);
            emit_field_name(info->fields[item->index].name, out);
            fputs(" = ", out);
            emit_static_value(item->value, out);
        }
        if (expr->as.init.count == 0) fputs("0", out);
        fputs(" }", out);
    } else {
        emit_expr(expr, out);
    }
}

/* Emits the constants that need storage and the top-level variables of one
   file. */
static void emit_data(const Unit *unit, FILE *out) {
    bool any = false;
    for (size_t i = 0; i < unit->symbols.count; i++) {
        const Symbol *symbol = unit->symbols.items[i];
        if (!is_stored_const(symbol)) continue;
        const Stmt *decl = symbol->decl;
        fprintf(out, "static const %s ", c_type(symbol->type));
        emit_symbol(symbol, out);
        if (symbol->is_array) {
            fprintf(out, "[%zu] = { ", symbol->length);
            for (size_t j = 0; j < decl->as.decl.elem_count; j++) {
                if (j) fputs(", ", out);
                emit_static_value(decl->as.decl.elems[j], out);
            }
            fputs(" };\n", out);
        } else {
            fputs(" = ", out);
            emit_static_value(decl->as.decl.init, out);
            fputs(";\n", out);
        }
        any = true;
    }
    emit_variables(&unit->program.globals, "static ", out);
    if (any || unit->program.globals.count) fputs("\n", out);
}

/* Declares an external function that no header declares. */
static void emit_external_prototype(const Stmt *func, FILE *out) {
    fprintf(out, "%s %.*s(", c_type(func->as.func.ret), (int)func->as.func.name.len,
            func->as.func.name.data);
    if (func->as.func.param_count == 0) fputs("void", out);
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        fprintf(out, "%s%s", i ? ", " : "", c_type(func->as.func.params[i].type));
    }
    fputs(");\n", out);
}

static void emit_prototypes(const Unit *unit, FILE *out) {
    const Block *top = &unit->program.top;
    bool any = false;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *func = top->stmts[i];
        if (func->kind != STMT_FUNC) continue;
        if (func->external && func->header.len) continue;
        if (func->external) {
            emit_external_prototype(func, out);
        } else {
            emit_signature(func, out);
            fputs(";\n", out);
        }
        any = true;
    }
    if (any) fputs("\n", out);
}

static void emit_functions(const Unit *unit, FILE *out) {
    const Block *top = &unit->program.top;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *func = top->stmts[i];
        if (func->kind != STMT_FUNC || func->external) continue;
        emit_signature(func, out);
        fputs(" {\n", out);
        emit_variables(&func->as.func.locals, "    ", out);
        bool outer = bounds_checked;
        if (func->bounds != BOUNDS_INHERIT) bounds_checked = func->bounds == BOUNDS_CHECKED;
        emit_block(&func->as.func.body, 1, out);
        bounds_checked = outer;
        fputs("}\n\n", out);
    }
}

/* Emits the program's files as one C file: the imported ones in an order
   that puts each after the files it imports, then the root. All data and
   prototypes come before any function body, so that functions can call each
   other in any order and across files. */
/* Emits the C function that calls the platform's repeat function once, if
   the target has one; it does nothing when the program does not define
   the function. */
static void emit_repeat(const Unit *unit, FILE *out) {
    if (!target->repeat) return;
    fprintf(out, "\n%s {\n", target->repeat);
    const char *name = target->platform.repeat_function;
    const Block *top = &unit->program.top;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *func = top->stmts[i];
        if (func->kind == STMT_FUNC && str_eq_cstr(func->as.func.name, name)) {
            fputs("    ", out);
            emit_symbol(func->as.func.symbol, out);
            fputs("();\n", out);
        }
    }
    fputs("}\n", out);
}

static void emit_program(const Unit *unit, FILE *out) {
    for (size_t i = 0; i < unit->module_count; i++) emit_data(unit->modules[i], out);
    emit_data(unit, out);
    for (size_t i = 0; i < unit->module_count; i++) emit_prototypes(unit->modules[i], out);
    emit_prototypes(unit, out);
    for (size_t i = 0; i < unit->module_count; i++) emit_functions(unit->modules[i], out);
    emit_functions(unit, out);

    const Block *top = &unit->program.top;
    const Stmt *wind_main = unit->program.main;
    bool takes_args = wind_main && wind_main->as.func.param_count > 0;
    fprintf(out, "%s {\n", takes_args ? target->start_with_arguments : target->start);
    emit_variables(&unit->program.locals, "    ", out);
    /* An imported file's top level holds only declarations; running it
       initializes the file's variables. */
    for (size_t i = 0; i < unit->module_count; i++) {
        emit_block(&unit->modules[i]->program.top, 1, out);
    }
    emit_block(top, 1, out);
    if (wind_main) {
        /* The value an int main returns is the process exit status, where
           the target has one. */
        bool returns_status = wind_main->as.func.ret == TYPE_INT &&
                              target->start_returns_status;
        fputs(returns_status ? "    return (int)" : "    ", out);
        emit_symbol(wind_main->as.func.symbol, out);
        if (takes_args) {
            used.helpers[HELPER_ARGS] = true;
            used.slices[TYPE_STRING] = true;
            fprintf(out, "((%s)argc, wind_args(argc, argv));\n", c_type(TYPE_INT));
        } else {
            fputs("();\n", out);
        }
        if (returns_status) {
            fputs("}\n", out);
            emit_repeat(unit, out);
            return;
        }
    }
    fputs(target->start_returns_status ? "    return 0;\n}\n" : "}\n", out);
    emit_repeat(unit, out);
}

/* Emits what the program's declared types need: their definitions, in the
   order they were declared, which puts each after the types of its fields;
   then the array-parameter types and the printing functions that the
   program turned out to use. Must run after the program is emitted, which
   settles what is used, and before the runtime is, which this adds to. */
static void emit_declared_types(FILE *out) {
    size_t count = types_count();
    /* Every struct is named before any is defined, so that a pointer can
       point to a struct defined further down, or to its own. */
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (info->is_enum) {
            used.headers |= HEADER_STDINT;
            fprintf(out, "typedef int32_t %s;\n", info->c_name);
        } else if (!info->is_pointer) {
            fprintf(out, "typedef struct %s %s;\n", info->c_name, info->c_name);
        }
    }
    /* A pointer type is built after the type it points to, so this order
       defines each of them after its target. */
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (info->is_pointer) {
            fprintf(out, "typedef %s *%s;\n", c_type(info->target), info->c_name);
        }
    }
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (info->is_enum || info->is_pointer) continue;
        fprintf(out, "struct %s {\n", info->c_name);
        for (size_t j = 0; j < info->field_count; j++) {
            fprintf(out, "    %s ", c_type(info->fields[j].type));
            emit_field_name(info->fields[j].name, out);
            fputs(";\n", out);
        }
        fputs("};\n", out);
    }
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->slice_used) continue;
        used.headers |= HEADER_STDDEF;
        fprintf(out, "typedef struct {\n    %s *data;\n    size_t len;\n} wind_slice_%s;\n",
                info->c_name, info->c_stem);
    }
    if (count) fputs("\n", out);

    /* Printing a struct prints its fields, which may be of types that
       nothing else prints, so the set is grown until it stops changing. */
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
            if (info->array_writer_used) info->writer_used = true;
            if (!info->writer_used || info->writer_emitted) continue;
            info->writer_emitted = true;
            changed = true;
            used.helpers[HELPER_PUT] = true;
            if (info->is_enum) used.helpers[HELPER_WRITE_INT] = true;
            if (info->is_pointer) {
                const char *cast;
                value_writer((TypeKind)(TYPE_COUNT + i), true, &cast);
            }
            for (size_t j = 0; j < info->field_count; j++) {
                const char *cast;
                value_writer(info->fields[j].type, true, &cast);
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->writer_used || info->is_pointer) continue;
        fprintf(out, "static void %s(%s value);\n", info->writer_name, info->c_name);
    }
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->writer_used || info->is_pointer) continue;
        fprintf(out, "static void %s(%s value) {\n", info->writer_name, info->c_name);
        if (info->is_enum) {
            /* A value prints as its name; when several names share a value,
               as the first of them; when none has it, as the number. */
            fputs("    switch (value) {\n", out);
            for (size_t j = 0; j < info->value_count; j++) {
                bool repeated = false;
                for (size_t k = 0; k < j; k++) {
                    if (info->values[k].value == info->values[j].value) repeated = true;
                }
                if (repeated) continue;
                fprintf(out, "    case %lld: wind_put(\"%.*s\", %zu); break;\n",
                        info->values[j].value, (int)info->values[j].name.len,
                        info->values[j].name.data, info->values[j].name.len);
            }
            fputs("    default: wind_write_int((long long)value); break;\n    }\n", out);
        } else {
            fprintf(out, "    wind_put(\"%s{\", %zu);\n", info->short_name,
                    strlen(info->short_name) + 1);
            for (size_t j = 0; j < info->field_count; j++) {
                const Field *field = &info->fields[j];
                const char *cast;
                const char *writer = value_writer(field->type, true, &cast);
                fprintf(out, "    wind_put(\"%s%.*s = \", %zu);\n", j ? ", " : "",
                        (int)field->name.len, field->name.data,
                        field->name.len + (j ? 5 : 3));
                fprintf(out, "    %s(%svalue.", writer, cast);
                emit_field_name(field->name, out);
                fputs(");\n", out);
            }
            fputs("    wind_put(\"}\", 1);\n", out);
        }
        fputs("}\n\n", out);
    }
    for (size_t i = 0; i < count; i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->array_writer_used) continue;
        const char *cast;
        const char *writer = value_writer((TypeKind)(TYPE_COUNT + i), true, &cast);
        used.headers |= HEADER_STDDEF;
        used.helpers[HELPER_PUT] = true;
        fprintf(out,
                "static void wind_write_array_%s(const %s *data, size_t len) {\n"
                "    wind_put(\"[\", 1);\n"
                "    for (size_t i = 0; i < len; i++) {\n"
                "        if (i) wind_put(\", \", 2);\n"
                "        %s(%sdata[i]);\n"
                "    }\n"
                "    wind_put(\"]\", 1);\n"
                "}\n\n",
                info->c_stem, info->c_name, writer, cast);
    }
}

static char *emit_to_memory(void (*emit)(const Unit *, FILE *), const Unit *unit,
                            size_t *len) {
    char *text = NULL;
    FILE *buffer = open_memstream(&text, len);
    if (!buffer) fatal("out of memory");
    emit(unit, buffer);
    if (fclose(buffer) != 0) fatal("out of memory");
    return text;
}

static void emit_types_of(const Unit *unit, FILE *out) {
    (void)unit;
    emit_declared_types(out);
}

void emit_c(const Unit *unit, FILE *out, EmitOptions options) {
    target = options.target ? options.target : &target_host;
    bounds_checked = !options.no_bounds_check;
    free(used.externals);
    memset(&used, 0, sizeof used);
    types_use((TypeTable *)&unit->types);
    for (size_t i = 0; i < types_count(); i++) {
        TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        info->writer_used = info->writer_emitted = false;
        info->slice_used = info->array_writer_used = false;
    }

    /* Written in the reverse of the order they appear in, since each part
       finds out what the parts before it must provide. */
    size_t program_len, types_len;
    char *program = emit_to_memory(emit_program, unit, &program_len);
    char *types = emit_to_memory(emit_types_of, unit, &types_len);
    if (target->prologue) fputs(target->prologue, out);
    emit_runtime(out);
    fwrite(types, 1, types_len, out);
    fwrite(program, 1, program_len, out);
    free(types);
    free(program);
}
