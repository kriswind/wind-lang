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
 *   through wind_index, which stops the program if the index is out of
 *   range, a negative one included, unless the build or a directive leaves
 *   that test out. A literal index into a fixed array is written as the
 *   position; unit_analyze has already turned one that counts back from the
 *   end into the position it names.
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
    bool lists[TYPE_COUNT];         /* growable arrays, by element type */
    bool array_writers[TYPE_COUNT]; /* by element type */
    bool helpers[HELPER_COUNT];
    unsigned headers; /* Header flags the program's own code relies on */
    /* The headers named by the external functions and constants the
       program uses, each once. */
    Str *externals;
    size_t external_count;
    size_t external_cap;
} used;

/* The libraries named by the external functions and constants the program
   uses, each once; kept apart from `used` since they outlast the program. */
static struct {
    char **items;
    size_t count;
    size_t cap;
} links;

static void use_link(Str name) {
    if (!name.len) return;
    for (size_t i = 0; i < links.count; i++) {
        if (str_eq_cstr(name, links.items[i])) return;
    }
    if (links.count == links.cap) {
        links.cap = links.cap ? links.cap * 2 : 4;
        links.items = xrealloc(links.items, links.cap * sizeof *links.items);
    }
    char *copy = xmalloc(name.len + 1);
    memcpy(copy, name.data, name.len);
    copy[name.len] = '\0';
    links.items[links.count++] = copy;
}

const char *const *emit_c_links(size_t *count) {
    *count = links.count;
    return (const char *const *)links.items;
}

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
        "/* Reports an index that is out of range, and stops the program.\n"
        "   is_signed tells whether the index had a signed type, which decides\n"
        "   how it is printed. */\n"
        "WIND_RARE static _Noreturn void wind_index_fail(long long i, int is_signed, size_t len, int line) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": index \");\n"
        "    wind_error_number(i, is_signed);\n"
        "    wind_error(\" is out of range for length \");\n"
        "    wind_error_number((long long)len, 0);\n"
        "    wind_error(\"\\n\");\n"
        "    wind_stop();\n"
        "}\n"
        "\n"
        "/* Turns an index into a position among len elements. A negative index\n"
        "   is out of range: one that counts back from the end is known at\n"
        "   compile time and arrives here as the position it names. */\n"
        "static inline size_t wind_index(long long i, int is_signed, size_t len, int line) {\n"
        "    if (WIND_UNLIKELY((unsigned long long)i >= len)) wind_index_fail(i, is_signed, len, line);\n"
        "    return (size_t)i;\n"
        "}\n",
        HEADER_STDDEF,
        {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
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
    [HELPER_SINK] = {
        "/* While a value is written into an array of char as text, wind_put\n"
        "   adds what it is given to the array, here, instead of writing it out. */\n"
        "#define WIND_SINK 1\n"
        "static wind_list_char *wind_sink;\n"
        "static int wind_sink_line;\n"
        "static _Noreturn void wind_out_of_memory(int line);\n"
        "\n"
        "static void wind_sink_put(const char *bytes, size_t count) {\n"
        "    wind_list_char *into = wind_sink;\n"
        "    if (into->len + count > into->cap) {\n"
        "        /* What is written may be the array's own text, which moves. */\n"
        "        const char *start = (const char *)into->data;\n"
        "        int own = start && bytes >= start && bytes < start + into->len;\n"
        "        size_t offset = own ? (size_t)(bytes - start) : 0;\n"
        "        size_t room = into->cap ? into->cap : 16;\n"
        "        while (room < into->len + count) room *= 2;\n"
        "        wind_char *grown = realloc(into->data, room);\n"
        "        if (!grown) wind_out_of_memory(wind_sink_line);\n"
        "        into->data = grown;\n"
        "        into->cap = room;\n"
        "        if (own) bytes = (const char *)grown + offset;\n"
        "    }\n"
        "    if (count) memcpy(into->data + into->len, bytes, count);\n"
        "    into->len += count;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB | HEADER_STRING, {HELPER_OUT_OF_MEMORY}, {TYPE_CHAR}},
    [HELPER_OUT_OF_MEMORY] = {
        "WIND_RARE static _Noreturn void wind_out_of_memory(int line) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": out of memory\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_GROW] = {
        "/* Makes room for need elements of size bytes in the block at data, which\n"
        "   has room for *cap, and returns where the elements now are. Room grows\n"
        "   by doubling, so that appending one at a time stays quick. */\n"
        "static void *wind_grow(void *data, size_t *cap, size_t need, size_t size, int line) {\n"
        "    if (need <= *cap) return data;\n"
        "    size_t room = *cap ? *cap : 4;\n"
        "    while (room < need) room *= 2;\n"
        "    void *grown = realloc(data, room * size);\n"
        "    if (!grown) wind_out_of_memory(line);\n"
        "    *cap = room;\n"
        "    return grown;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB, {HELPER_OUT_OF_MEMORY}, {0}},
    [HELPER_CLONE] = {
        "/* A copy of len elements of size bytes from data, on the heap. */\n"
        "static void *wind_clone(const void *data, size_t len, size_t size, int line) {\n"
        "    if (!len) return NULL;\n"
        "    void *copy = malloc(len * size);\n"
        "    if (!copy) wind_out_of_memory(line);\n"
        "    memcpy(copy, data, len * size);\n"
        "    return copy;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB | HEADER_STRING, {HELPER_OUT_OF_MEMORY}, {0}},
    [HELPER_POP_CHECK] = {
        "/* Stops the program if there is no element to pop. */\n"
        "static void wind_pop_check(size_t len, int line) {\n"
        "    if (len) return;\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": pop from an empty array\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        HEADER_STDDEF, {HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_MAP] = {
        "/* A map: its entries in the order they were first put, and a hash table\n"
        "   of where they are. A removed entry stays where it was, marked as no\n"
        "   longer live, until the table is rebuilt. A string key's characters\n"
        "   are copied, so that the map never holds a view of text that is gone. */\n"
        "typedef struct {\n"
        "    void *keys;\n"
        "    void *values;\n"
        "    unsigned char *live;\n"
        "    size_t used;       /* entries used, removed ones included */\n"
        "    size_t cap;        /* entries there is room for */\n"
        "    size_t len;        /* entries live */\n"
        "    size_t *slots;     /* the table: an entry's position + 1, or 0 */\n"
        "    size_t slot_count; /* a power of two, or 0 */\n"
        "} wind_map;\n",
        HEADER_STDDEF, {0}, {TYPE_STRING}},
    [HELPER_MAP_FIND] = {
        "static size_t wind_map_hash(const void *key, size_t size, int is_string) {\n"
        "    const unsigned char *bytes = key;\n"
        "    size_t count = size;\n"
        "    if (is_string) {\n"
        "        const wind_string *s = key;\n"
        "        bytes = (const unsigned char *)s->data;\n"
        "        count = s->len;\n"
        "    }\n"
        "    unsigned long hash = 2166136261UL;\n"
        "    for (size_t i = 0; i < count; i++) hash = (hash ^ bytes[i]) * 16777619UL;\n"
        "    return (size_t)hash;\n"
        "}\n"
        "\n"
        "static int wind_map_equal(const void *a, const void *b, size_t size, int is_string) {\n"
        "    if (!is_string) return memcmp(a, b, size) == 0;\n"
        "    const wind_string *x = a, *y = b;\n"
        "    return x->len == y->len && (!x->len || memcmp(x->data, y->data, x->len) == 0);\n"
        "}\n"
        "\n"
        "/* The position of the live entry with this key, or (size_t)-1. */\n"
        "static size_t wind_map_find(const wind_map *m, const void *key, size_t key_size, int is_string) {\n"
        "    if (!m->slot_count) return (size_t)-1;\n"
        "    size_t mask = m->slot_count - 1;\n"
        "    for (size_t at = wind_map_hash(key, key_size, is_string) & mask;; at = (at + 1) & mask) {\n"
        "        size_t entry = m->slots[at];\n"
        "        if (!entry) return (size_t)-1;\n"
        "        entry--;\n"
        "        if (m->live[entry] &&\n"
        "            wind_map_equal((const char *)m->keys + entry * key_size, key, key_size, is_string)) {\n"
        "            return entry;\n"
        "        }\n"
        "    }\n"
        "}\n",
        HEADER_STDDEF | HEADER_STRING, {HELPER_MAP}, {0}},
    [HELPER_MAP_PUT] = {
        "static void wind_map_slot(wind_map *m, size_t entry, size_t key_size, int is_string) {\n"
        "    size_t mask = m->slot_count - 1;\n"
        "    size_t at = wind_map_hash((char *)m->keys + entry * key_size, key_size, is_string) & mask;\n"
        "    while (m->slots[at]) at = (at + 1) & mask;\n"
        "    m->slots[at] = entry + 1;\n"
        "}\n"
        "\n"
        "/* Moves the live entries together, in order, and makes the table anew\n"
        "   with room for twice as many again, so that at most half of it is in use. */\n"
        "static void wind_map_rebuild(wind_map *m, size_t key_size, size_t value_size, int is_string, int line) {\n"
        "    size_t kept = 0;\n"
        "    for (size_t i = 0; i < m->used; i++) {\n"
        "        if (!m->live[i]) continue;\n"
        "        if (kept != i) {\n"
        "            memcpy((char *)m->keys + kept * key_size, (char *)m->keys + i * key_size, key_size);\n"
        "            memcpy((char *)m->values + kept * value_size, (char *)m->values + i * value_size, value_size);\n"
        "            m->live[kept] = 1;\n"
        "        }\n"
        "        kept++;\n"
        "    }\n"
        "    m->used = kept;\n"
        "    size_t count = 8;\n"
        "    while (count < (kept + 1) * 4) count *= 2;\n"
        "    free(m->slots);\n"
        "    m->slots = calloc(count, sizeof *m->slots);\n"
        "    if (!m->slots) wind_out_of_memory(line);\n"
        "    m->slot_count = count;\n"
        "    for (size_t i = 0; i < kept; i++) wind_map_slot(m, i, key_size, is_string);\n"
        "}\n"
        "\n"
        "/* The value stored under key, put there as zero if the key is new. */\n"
        "static void *wind_map_put(wind_map *m, const void *key, size_t key_size, size_t value_size,\n"
        "                          int is_string, int line) {\n"
        "    size_t found = wind_map_find(m, key, key_size, is_string);\n"
        "    if (found != (size_t)-1) return (char *)m->values + found * value_size;\n"
        "    if ((m->used + 1) * 2 > m->slot_count) wind_map_rebuild(m, key_size, value_size, is_string, line);\n"
        "    if (m->used == m->cap) {\n"
        "        size_t cap = m->cap ? m->cap * 2 : 8;\n"
        "        void *keys = realloc(m->keys, cap * key_size);\n"
        "        if (!keys) wind_out_of_memory(line);\n"
        "        m->keys = keys;\n"
        "        void *values = realloc(m->values, cap * value_size);\n"
        "        if (!values) wind_out_of_memory(line);\n"
        "        m->values = values;\n"
        "        unsigned char *live = realloc(m->live, cap);\n"
        "        if (!live) wind_out_of_memory(line);\n"
        "        m->live = live;\n"
        "        m->cap = cap;\n"
        "    }\n"
        "    size_t entry = m->used++;\n"
        "    char *stored = (char *)m->keys + entry * key_size;\n"
        "    memcpy(stored, key, key_size);\n"
        "    if (is_string) {\n"
        "        wind_string *s = (wind_string *)stored;\n"
        "        char *copy = NULL;\n"
        "        if (s->len) {\n"
        "            copy = malloc(s->len);\n"
        "            if (!copy) wind_out_of_memory(line);\n"
        "            memcpy(copy, s->data, s->len);\n"
        "        }\n"
        "        s->data = copy;\n"
        "    }\n"
        "    memset((char *)m->values + entry * value_size, 0, value_size);\n"
        "    m->live[entry] = 1;\n"
        "    m->len++;\n"
        "    wind_map_slot(m, entry, key_size, is_string);\n"
        "    return (char *)m->values + entry * value_size;\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB | HEADER_STRING, {HELPER_MAP_FIND, HELPER_OUT_OF_MEMORY}, {0}},
    [HELPER_MAP_REMOVE] = {
        "static void wind_map_remove(wind_map *m, const void *key, size_t key_size, int is_string) {\n"
        "    size_t found = wind_map_find(m, key, key_size, is_string);\n"
        "    if (found == (size_t)-1) return;\n"
        "    m->live[found] = 0;\n"
        "    m->len--;\n"
        "    if (is_string) free((void *)((wind_string *)((char *)m->keys + found * key_size))->data);\n"
        "}\n",
        HEADER_STDLIB, {HELPER_MAP_FIND}, {0}},
    [HELPER_MAP_CLEAR] = {
        "/* Removes every entry, keeping the room. */\n"
        "static void wind_map_clear(wind_map *m, size_t key_size, int is_string) {\n"
        "    for (size_t i = 0; is_string && i < m->used; i++) {\n"
        "        if (m->live[i]) free((void *)((wind_string *)((char *)m->keys + i * key_size))->data);\n"
        "    }\n"
        "    m->used = m->len = 0;\n"
        "    if (m->slot_count) memset(m->slots, 0, m->slot_count * sizeof *m->slots);\n"
        "}\n",
        HEADER_STDLIB | HEADER_STRING, {HELPER_MAP}, {0}},
    [HELPER_MAP_DELETE] = {
        "static void wind_map_delete(wind_map *m, size_t key_size, int is_string) {\n"
        "    wind_map_clear(m, key_size, is_string);\n"
        "    free(m->keys);\n"
        "    free(m->values);\n"
        "    free(m->live);\n"
        "    free(m->slots);\n"
        "    memset(m, 0, sizeof *m);\n"
        "}\n",
        HEADER_STDLIB | HEADER_STRING, {HELPER_MAP_CLEAR}, {0}},
    [HELPER_PAD] = {
        "/* Writes the count bytes of text with spaces before them, or after\n"
        "   them if left, to make width characters; a character is counted\n"
        "   once, however many bytes it takes. */\n"
        "static void wind_pad(const wind_char *text, size_t count, long long width, int left) {\n"
        "    long long chars = 0;\n"
        "    for (size_t i = 0; i < count; i++) chars += (text[i] & 0xC0) != 0x80;\n"
        "    long long spaces = width > chars ? width - chars : 0;\n"
        "    for (long long i = 0; !left && i < spaces; i++) wind_put(\" \", 1);\n"
        "    wind_put((const char *)text, count);\n"
        "    for (long long i = 0; left && i < spaces; i++) wind_put(\" \", 1);\n"
        "}\n",
        HEADER_STDDEF, {HELPER_PUT}, {TYPE_CHAR}},
    [HELPER_SLICE] = {
        "WIND_RARE static _Noreturn void wind_slice_fail(long long from, long long to, size_t len, int line) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": slice from \");\n"
        "    wind_error_number(from, 1);\n"
        "    wind_error(\" to \");\n"
        "    wind_error_number(to, 1);\n"
        "    wind_error(\" is out of range for length \");\n"
        "    wind_error_number((long long)len, 0);\n"
        "    wind_error(\"\\n\");\n"
        "    wind_stop();\n"
        "}\n"
        "\n"
        "/* The part of s from position from up to position to, or to the end\n"
        "   without has_to. With check, the positions must be in order and in s. */\n"
        "static inline wind_string wind_slice(wind_string s, long long from, long long to,\n"
        "                                     int has_to, int check, int line) {\n"
        "    if (!has_to) to = (long long)s.len;\n"
        "    if (check && (from < 0 || to < from || (unsigned long long)to > s.len)) {\n"
        "        wind_slice_fail(from, to, s.len, line);\n"
        "    }\n"
        "    wind_string part = { s.data + from, (size_t)(to - from) };\n"
        "    return part;\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {TYPE_STRING}},
    [HELPER_STRING_AT] = {
        "static wind_char wind_string_at(wind_string s, long long i, int is_signed, int line) {\n"
        "    return (wind_char)s.data[wind_index(i, is_signed, s.len, line)];\n"
        "}\n",
        0, {HELPER_INDEX}, {TYPE_STRING, TYPE_CHAR}},
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
    use_link(decl->link);
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

/* The C name of a symbol, as it is declared. */
static void emit_symbol_name(const Symbol *symbol, FILE *out) {
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

/* A symbol as it is used. A growable array parameter is a pointer to the
   caller's array, used here as the array itself. */
static void emit_symbol(const Symbol *symbol, FILE *out) {
    bool through = symbol->kind == SYM_PARAM && (symbol->is_growable || symbol->is_map);
    if (through) fputs("(*", out);
    emit_symbol_name(symbol, out);
    if (through) fputs(")", out);
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

/* The type of a growable array of an element type, noted as needing its
   definition. Its data and len come first, as in a slice. */
static void emit_list_type(TypeKind elem, FILE *out) {
    TypeInfo *info = type_info(elem);
    if (info) {
        info->list_used = true;
        fprintf(out, "wind_list_%s", info->c_stem);
        return;
    }
    used.lists[elem] = true;
    fprintf(out, "wind_list_%s", type_name(elem));
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
           any_array(expr->as.ident.symbol);
}

/* Emits a pointer, checked for NULL, as a pointer of its own type again. */
static void emit_checked_pointer(const Expr *pointer, FILE *out) {
    used.helpers[HELPER_DEREF] = true;
    fprintf(out, "((%s)wind_deref(", c_type(pointer->type));
    emit_expr(pointer, out);
    fprintf(out, ", %d))", pointer->loc.line);
}

/* If the index is a literal within a fixed array's length, sets *position
   to the element it names and returns true. Such an index needs nothing
   worked out when the program runs. */
static bool literal_position(const Symbol *array, const Expr *index, size_t *position) {
    if (runtime_length(array) || index->kind != EXPR_INT_LIT) return false;
    if (index->as.int_lit >= array->length) return false;
    *position = (size_t)index->as.int_lit;
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
    emit_symbol(array, out);
    if (runtime_length(array)) fputs(".data", out);
    if (!bounds_checked) {
        fputs("[(size_t)(", out);
        emit_expr(index, out);
        fputs(")]", out);
        return;
    }
    used.helpers[HELPER_INDEX] = true;
    fputs("[wind_index((long long)(", out);
    emit_expr(index, out);
    fprintf(out, "), %d, ", type_is_signed(index->type));
    if (runtime_length(array)) {
        emit_symbol(array, out);
        fputs(".len", out);
    } else {
        fprintf(out, "%zu", array->length);
    }
    fprintf(out, ", %d)]", index->loc.line);
}

static void emit_int_value(U128 value, TypeKind type, FILE *out);

static void emit_int_literal(const Expr *expr, FILE *out) {
    emit_int_value(expr->as.int_lit, expr->type, out);
}

/* Writes value converted to type. A negative value arrives as its two's
   complement, which is how the conversion reads it back. */
static void emit_int_value(U128 value, TypeKind type, FILE *out) {
    uint64_t low = (uint64_t)value;
    uint64_t high = (uint64_t)(value >> 64);
    fprintf(out, "((%s)", c_type(type));
    if (high == 0) {
        fprintf(out, "%lluULL)", (unsigned long long)low);
    } else if (high == UINT64_MAX && low >> 63) {
        /* A negative value that 64 bits hold, which needs no wider type. */
        uint64_t magnitude = -low;
        if (magnitude >> 63) {
            fputs("(-9223372036854775807LL - 1))", out);
        } else {
            fprintf(out, "(-%lldLL))", (long long)magnitude);
        }
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

/* ---- Order of evaluation ----

   Wind evaluates the operands of an operator, the arguments of a call and
   the values of an initializer from left to right. C leaves that order
   unspecified, which is seen only when a call changes something another
   operand reads, or two calls both have effects. Where that can happen,
   the operands are evaluated one after another into temporaries, inside a
   GNU statement expression, and the expression is made from those. */

/* Operands held in temporaries, which emit_expr writes as their names. */
static struct {
    const Expr **items;
    int *ids;
    bool *through; /* written "(*wind_pN)": a pointer to the place */
    size_t count;
    size_t cap;
    int next_id;
} held;

/* Has expr written as a temporary from here on, until held.count is set
   back: "wind_tN", or "(*wind_pN)" if through. */
static void hold(const Expr *expr, int id, bool through) {
    if (held.count == held.cap) {
        held.cap = held.cap ? held.cap * 2 : 16;
        held.items = xrealloc(held.items, held.cap * sizeof *held.items);
        held.ids = xrealloc(held.ids, held.cap * sizeof *held.ids);
        held.through = xrealloc(held.through, held.cap * sizeof *held.through);
    }
    held.items[held.count] = expr;
    held.ids[held.count] = id;
    held.through[held.count++] = through;
}

static bool has_call(const Expr *expr) {
    switch (expr->kind) {
    case EXPR_CALL:
        return true;
    case EXPR_INDEX:
        return has_call(expr->as.index.base) || has_call(expr->as.index.index);
    case EXPR_SLICE:
        return has_call(expr->as.slice.base) ||
               (expr->as.slice.from && has_call(expr->as.slice.from)) ||
               (expr->as.slice.to && has_call(expr->as.slice.to));
    case EXPR_LEN:
        return has_call(expr->as.len.operand);
    case EXPR_FIELD:
        return has_call(expr->as.field.base);
    case EXPR_INIT:
        for (size_t i = 0; i < expr->as.init.count; i++) {
            if (has_call(expr->as.init.items[i].value)) return true;
        }
        return false;
    case EXPR_UNARY:
        return has_call(expr->as.unary.operand);
    case EXPR_BINARY:
        return has_call(expr->as.binary.left) || has_call(expr->as.binary.right);
    case EXPR_CAST:
        return has_call(expr->as.cast.operand);
    case EXPR_TERNARY:
        return has_call(expr->as.ternary.cond) || has_call(expr->as.ternary.then) ||
               has_call(expr->as.ternary.otherwise);
    default:
        return false;
    }
}

/* True for an expression whose value no call can change: a constant, a
   whole array, or a local variable or parameter that nothing points into,
   and what is computed from those alone. */
static bool is_stable(const Expr *expr) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_NULL:
    case EXPR_SIZEOF:
        return true;
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (symbol->kind != SYM_VAR && symbol->kind != SYM_PARAM) return true;
        if (any_array(symbol) || symbol->is_map) return true;
        return !symbol->global && !symbol->address_taken;
    }
    case EXPR_LEN:
        /* A growable array's or a map's length changes as it grows. */
        if (is_array_name(expr->as.len.operand)) {
            return !expr->as.len.operand->as.ident.symbol->is_growable;
        }
        return expr->as.len.operand->kind != EXPR_IDENT || !expr->as.len.operand->as.ident.symbol ||
                       !expr->as.len.operand->as.ident.symbol->is_map
                   ? is_stable(expr->as.len.operand)
                   : false;
    case EXPR_UNARY:
        if (expr->as.unary.op == OP_ADDR) return expr->as.unary.operand->kind == EXPR_IDENT;
        return expr->as.unary.op != OP_DEREF && is_stable(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_stable(expr->as.binary.left) && is_stable(expr->as.binary.right);
    case EXPR_CAST:
        /* A view of a growable array's text changes with it. */
        if (expr->type == TYPE_STRING && is_array_name(expr->as.cast.operand)) return false;
        return is_stable(expr->as.cast.operand);
    case EXPR_TERNARY:
        return is_stable(expr->as.ternary.cond) && is_stable(expr->as.ternary.then) &&
               is_stable(expr->as.ternary.otherwise);
    default:
        return false;
    }
}

/* True if evaluating these operands in some other order than the one
   given could change what they come to. */
static bool order_matters(const Expr *const *operands, size_t count) {
    bool call = false;
    size_t unstable = 0;
    for (size_t i = 0; i < count; i++) {
        if (has_call(operands[i])) call = true;
        if (!is_stable(operands[i])) unstable++;
    }
    return call && unstable >= 2;
}

/* Opens a statement expression that evaluates the operands that are not
   stable, in order, into temporaries that stand for them until
   release_operands. Returns what to pass to it. */
static size_t hold_operands(const Expr *const *operands, size_t count, FILE *out) {
    size_t mark = held.count;
    fputs("__extension__ ({ ", out);
    for (size_t i = 0; i < count; i++) {
        const Expr *operand = operands[i];
        if (is_stable(operand)) continue;
        int id = ++held.next_id;
        fprintf(out, "%s wind_t%d = ", c_type(operand->type), id);
        emit_expr(operand, out);
        fputs("; ", out);
        hold(operand, id, false);
    }
    return mark;
}

static void release_operands(size_t mark, FILE *out) {
    fputs("; })", out);
    held.count = mark;
}

/* The operands of expr whose order C leaves open, from malloc. */
static size_t unordered_operands(const Expr *expr, const Expr ***operands) {
    size_t count = 0;
    *operands = NULL;
    switch (expr->kind) {
    case EXPR_CALL:
        /* A built-in function works out its arguments in order itself. */
        if (expr->as.call.builtin) break;
        count = expr->as.call.count;
        *operands = xmalloc((count ? count : 1) * sizeof **operands);
        for (size_t i = 0; i < count; i++) (*operands)[i] = expr->as.call.args[i];
        break;
    case EXPR_INIT:
        count = expr->as.init.count;
        *operands = xmalloc((count ? count : 1) * sizeof **operands);
        for (size_t i = 0; i < count; i++) (*operands)[i] = expr->as.init.items[i].value;
        break;
    case EXPR_BINARY:
        if (expr->as.binary.op == OP_AND || expr->as.binary.op == OP_OR) break;
        count = 2;
        *operands = xmalloc(2 * sizeof **operands);
        (*operands)[0] = expr->as.binary.left;
        (*operands)[1] = expr->as.binary.right;
        break;
    case EXPR_INDEX:
        if (is_array_name(expr->as.index.base)) break;
        count = 2;
        *operands = xmalloc(2 * sizeof **operands);
        (*operands)[0] = expr->as.index.base;
        (*operands)[1] = expr->as.index.index;
        break;
    case EXPR_SLICE:
        *operands = xmalloc(3 * sizeof **operands);
        (*operands)[count++] = expr->as.slice.base;
        if (expr->as.slice.from) (*operands)[count++] = expr->as.slice.from;
        if (expr->as.slice.to) (*operands)[count++] = expr->as.slice.to;
        break;
    default:
        break;
    }
    return count;
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

/* A new growable array of elem holding a copy of source's elements, or of
   a string's bytes. */
static void emit_clone(const Expr *source, TypeKind elem, int line, FILE *out) {
    used.helpers[HELPER_CLONE] = true;
    if (!is_array_name(source)) {
        int id = ++held.next_id;
        fprintf(out, "__extension__ ({ %s wind_s%d = ", c_type(TYPE_STRING), id);
        emit_expr(source, out);
        fputs("; (", out);
        emit_list_type(elem, out);
        fprintf(out, "){ wind_clone(wind_s%d.data, wind_s%d.len, 1, %d), wind_s%d.len, "
                     "wind_s%d.len }; })",
                id, id, line, id, id);
        return;
    }
    const Symbol *array = source->as.ident.symbol;
    fputs("(", out);
    emit_list_type(elem, out);
    fputs("){ wind_clone(", out);
    emit_symbol(array, out);
    if (runtime_length(array)) {
        fputs(".data, ", out);
        emit_symbol(array, out);
        fprintf(out, ".len, sizeof(%s), %d), ", c_type(elem), line);
        emit_symbol(array, out);
        fputs(".len, ", out);
        emit_symbol(array, out);
        fputs(".len }", out);
    } else {
        fprintf(out, ", %zu, sizeof(%s), %d), %zu, %zu }", array->length, c_type(elem), line,
                array->length, array->length);
    }
}

static void emit_write(const Expr *arg, FILE *out);

static bool is_format_call(const Expr *expr) {
    if (expr->kind != EXPR_CALL) return false;
    Builtin which = expr->as.call.builtin;
    return which == BUILTIN_LEFT || which == BUILTIN_RIGHT || which == BUILTIN_FIXED;
}

static void use_sink(void) {
    used.helpers[HELPER_SINK] = true;
    used.lists[TYPE_CHAR] = true;
}

/* Works out a value into a temporary that stands for it from here on. */
static void hold_value(const Expr *expr, FILE *out) {
    if (is_stable(expr)) return;
    int id = ++held.next_id;
    fprintf(out, "%s wind_t%d = ", c_type(expr->type), id);
    emit_expr(expr, out);
    fputs("; ", out);
    hold(expr, id, false);
}

/* Works out what is written by arg, a format's parts included. */
static void hold_written(const Expr *arg, FILE *out) {
    if (is_array_name(arg) || (arg->kind == EXPR_IDENT && arg->as.ident.symbol &&
                               arg->as.ident.symbol->is_map)) {
        return;
    }
    if (is_format_call(arg)) {
        if (arg->as.call.builtin == BUILTIN_FIXED) {
            hold_value(arg->as.call.args[0], out);
        } else {
            hold_written(arg->as.call.args[0], out);
        }
        hold_value(arg->as.call.args[1], out);
        return;
    }
    hold_value(arg, out);
}

/* Writes args as text into the array of char that the C expression into
   points to. The values are worked out first, in order, so that nothing
   they print themselves lands in it. With nul, a '\0' ends the text. */
static void emit_written_into(Expr *const *args, size_t count, const char *into, int line,
                              bool nul, FILE *out) {
    use_sink();
    int id = ++held.next_id;
    size_t mark = held.count;
    fputs("{ ", out);
    for (size_t i = 0; i < count; i++) hold_written(args[i], out);
    fprintf(out, "wind_list_char *wind_outer%d = wind_sink; wind_sink = %s; "
                 "wind_sink_line = %d;\n",
            id, into, line);
    for (size_t i = 0; i < count; i++) emit_write(args[i], out);
    if (nul) fputs("wind_put(\"\", 1);\n", out);
    fprintf(out, "wind_sink = wind_outer%d; }", id);
    held.count = mark;
}

/* left, right and fixed, as cout or a sink writes them. */
static void emit_format(const Expr *expr, FILE *out) {
    const Expr *value = expr->as.call.args[0];
    const Expr *amount = expr->as.call.args[1];
    if (expr->as.call.builtin == BUILTIN_FIXED) {
        used.helpers[HELPER_WRITE_FIXED] = true;
        fputs("wind_write_fixed((long double)(", out);
        emit_expr(value, out);
        fputs("), (int)(", out);
        emit_expr(amount, out);
        fputs("));\n", out);
        return;
    }
    /* The value is written into a scratch array first, to be measured. */
    use_sink();
    used.helpers[HELPER_PAD] = true;
    used.headers |= HEADER_STDLIB;
    int id = ++held.next_id;
    fprintf(out, "{ wind_list_char *wind_outer%d = wind_sink; wind_list_char wind_f%d = {0}; "
                 "wind_sink = &wind_f%d;\n",
            id, id, id);
    emit_write(value, out);
    fprintf(out, "wind_sink = wind_outer%d; wind_pad(wind_f%d.data, wind_f%d.len, (long long)(",
            id, id, id);
    emit_expr(amount, out);
    fprintf(out, "), %d); free(wind_f%d.data); }\n", expr->as.call.builtin == BUILTIN_LEFT,
            id);
}

/* The map an expression names, if it is a map's name. */
static const Symbol *map_named(const Expr *expr) {
    return expr->kind == EXPR_IDENT && expr->as.ident.symbol && expr->as.ident.symbol->is_map
               ? expr->as.ident.symbol
               : NULL;
}

/* "sizeof(K), is_string", what the map functions are told of a key. */
static void emit_key_facts(const Symbol *map, FILE *out) {
    fprintf(out, "sizeof(%s), %d", c_type(map->key_type), map->key_type == TYPE_STRING);
}

/* Declares "K wind_kN = key; " for a map's key. */
static void emit_key_temp(const Symbol *map, const Expr *key, int id, FILE *out) {
    fprintf(out, "%s wind_k%d = ", c_type(map->key_type), id);
    emit_expr(key, out);
    fputs("; ", out);
}

/* Looking up a key: the value stored under it, or zero. */
static void emit_map_read(const Symbol *map, const Expr *key, FILE *out) {
    used.helpers[HELPER_MAP_FIND] = true;
    int id = ++held.next_id;
    fputs("__extension__ ({ ", out);
    emit_key_temp(map, key, id, out);
    fprintf(out, "size_t wind_i%d = wind_map_find(&", id);
    emit_symbol(map, out);
    fprintf(out, ", &wind_k%d, ", id);
    emit_key_facts(map, out);
    fprintf(out, "); wind_i%d == (size_t)-1 ? (%s){0} : ((%s *)", id, c_type(map->type),
            c_type(map->type));
    emit_symbol(map, out);
    fprintf(out, ".values)[wind_i%d]; })", id);
}

/* has, remove, clear and delete on a map. */
static void emit_map_builtin(const Expr *expr, const Symbol *map, FILE *out) {
    int id = ++held.next_id;
    Builtin which = expr->as.call.builtin;
    used.helpers[which == BUILTIN_HAS      ? HELPER_MAP_FIND
                 : which == BUILTIN_REMOVE ? HELPER_MAP_REMOVE
                 : which == BUILTIN_CLEAR  ? HELPER_MAP_CLEAR
                                           : HELPER_MAP_DELETE] = true;
    fputs("__extension__ ({ ", out);
    if (which == BUILTIN_HAS || which == BUILTIN_REMOVE) {
        emit_key_temp(map, expr->as.call.args[1], id, out);
        fprintf(out, which == BUILTIN_HAS ? "(%s)(wind_map_find(&" : "wind_map_remove(&",
                c_type(TYPE_BOOL));
        emit_symbol(map, out);
        fprintf(out, ", &wind_k%d, ", id);
        emit_key_facts(map, out);
        fputs(which == BUILTIN_HAS ? ") != (size_t)-1); })" : "); (void)0; })", out);
        return;
    }
    fputs(which == BUILTIN_CLEAR ? "wind_map_clear(&" : "wind_map_delete(&", out);
    emit_symbol(map, out);
    fputs(", ", out);
    emit_key_facts(map, out);
    fputs("); (void)0; })", out);
}

/* A call of one of the built-in functions on growable arrays. Those that
   give no value are statement expressions ending in "(void)0". */
static void emit_builtin(const Expr *expr, FILE *out) {
    if (expr->as.call.builtin == BUILTIN_EXIT) {
        used.helpers[HELPER_EXIT] = true;
        fputs("wind_exit((int)(", out);
        emit_expr(expr->as.call.args[0], out);
        fputs("))", out);
        return;
    }
    const Symbol *map = map_named(expr->as.call.args[0]);
    if (map) {
        emit_map_builtin(expr, map, out);
        return;
    }
    const Symbol *array = expr->as.call.args[0]->as.ident.symbol;
    int line = expr->loc.line;
    int id = ++held.next_id;
    fputs("__extension__ ({ ", out);
    if (expr->as.call.builtin == BUILTIN_APPEND && array->type == TYPE_CHAR) {
        /* Into an array of char, each value is written as text. */
        use_sink();
        fprintf(out, "wind_list_char *wind_in%d = &", id);
        emit_symbol(array, out);
        fputs("; ", out);
        char into[32];
        snprintf(into, sizeof into, "wind_in%d", id);
        emit_written_into(expr->as.call.args + 1, expr->as.call.count - 1, into, line, false,
                          out);
        fputs(" (void)0; })", out);
        return;
    }
    if (expr->as.call.builtin == BUILTIN_APPEND) {
        /* The values first, in order, since they may read the array. */
        size_t count = expr->as.call.count - 1;
        for (size_t i = 0; i < count; i++) {
            fprintf(out, "%s wind_a%d_%zu = ", c_type(array->type), id, i);
            emit_expr(expr->as.call.args[i + 1], out);
            fputs("; ", out);
        }
        used.helpers[HELPER_GROW] = true;
        emit_list_type(array->type, out);
        fprintf(out, " *wind_d%d = &", id);
        emit_symbol(array, out);
        fprintf(out, "; wind_d%d->data = wind_grow(wind_d%d->data, &wind_d%d->cap, "
                     "wind_d%d->len + %zu, sizeof *wind_d%d->data, %d); ",
                id, id, id, id, count, id, line);
        for (size_t i = 0; i < count; i++) {
            fprintf(out, "wind_d%d->data[wind_d%d->len++] = wind_a%d_%zu; ", id, id, id, i);
        }
        fputs("(void)0; })", out);
        return;
    }
    emit_list_type(array->type, out);
    fprintf(out, " *wind_d%d = &", id);
    emit_symbol(array, out);
    fputs("; ", out);
    switch (expr->as.call.builtin) {
    case BUILTIN_POP:
        if (bounds_checked) {
            used.helpers[HELPER_POP_CHECK] = true;
            fprintf(out, "wind_pop_check(wind_d%d->len, %d); ", id, line);
        }
        fprintf(out, "wind_d%d->data[--wind_d%d->len]; })", id, id);
        return;
    case BUILTIN_CLEAR:
        fprintf(out, "wind_d%d->len = 0; (void)0; })", id);
        return;
    case BUILTIN_RESERVE:
        used.helpers[HELPER_GROW] = true;
        fprintf(out, "long long wind_n%d = (long long)(", id);
        emit_expr(expr->as.call.args[1], out);
        fprintf(out, "); if (wind_n%d > 0) wind_d%d->data = wind_grow(wind_d%d->data, "
                     "&wind_d%d->cap, (size_t)wind_n%d, sizeof *wind_d%d->data, %d); "
                     "(void)0; })",
                id, id, id, id, id, id, line);
        return;
    case BUILTIN_DELETE:
        used.headers |= HEADER_STDLIB;
        fprintf(out, "free(wind_d%d->data); wind_d%d->data = NULL; "
                     "wind_d%d->len = wind_d%d->cap = 0; (void)0; })",
                id, id, id, id);
        return;
    default:
        fputs("(void)0; })", out);
        return;
    }
}

static void emit_expr_kind(const Expr *expr, FILE *out) {
    switch (expr->kind) {
    case EXPR_INDEX: {
        const Expr *base = expr->as.index.base;
        if (map_named(base)) {
            emit_map_read(map_named(base), expr->as.index.index, out);
            break;
        }
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
        if (!bounds_checked) {
            fprintf(out, "((%s)(", c_type(TYPE_CHAR));
            emit_expr(base, out);
            fputs(").data[(size_t)(", out);
            emit_expr(expr->as.index.index, out);
            fputs(")])", out);
            break;
        }
        used.helpers[HELPER_STRING_AT] = true;
        fputs("wind_string_at(", out);
        emit_expr(base, out);
        fputs(", (long long)(", out);
        emit_expr(expr->as.index.index, out);
        fprintf(out, "), %d, %d)", type_is_signed(expr->as.index.index->type),
                expr->loc.line);
        break;
    }
    case EXPR_SLICE:
        used.helpers[HELPER_SLICE] = true;
        fputs("wind_slice(", out);
        emit_expr(expr->as.slice.base, out);
        fputs(", (long long)(", out);
        if (expr->as.slice.from) {
            emit_expr(expr->as.slice.from, out);
        } else {
            fputs("0", out);
        }
        fputs("), (long long)(", out);
        if (expr->as.slice.to) {
            emit_expr(expr->as.slice.to, out);
        } else {
            fputs("0", out);
        }
        fprintf(out, "), %d, %d, %d)", expr->as.slice.to != NULL, bounds_checked,
                expr->loc.line);
        break;
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
        } else if (runtime_length(operand->as.ident.symbol)) {
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
        if (expr->as.unary.op == OP_NEG && expr->as.unary.operand->kind == EXPR_INT_LIT &&
            type_is_integer(expr->type)) {
            /* Folded, so that the most negative value is not written as the
               negation of a number its type cannot hold. */
            emit_int_value(-expr->as.unary.operand->as.int_lit, expr->type, out);
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
        if (expr->as.call.builtin) {
            emit_builtin(expr, out);
            break;
        }
        /* Pointers cross into and out of C through void *, since C's own
           declaration may differ in what it points to (char against the
           unsigned char of Wind, or const), and Wind has already checked
           them against the extern declaration. */
        bool external = expr->as.call.symbol->decl->external;
        bool cast_result = external && type_is_pointer(expr->type);
        if (cast_result) fprintf(out, "((%s)(void *)", c_type(expr->type));
        emit_symbol(expr->as.call.symbol, out);
        fputs("(", out);
        const Stmt *func = expr->as.call.symbol->decl;
        for (size_t i = 0; i < expr->as.call.count; i++) {
            const Expr *arg = expr->as.call.args[i];
            if (i) fputs(", ", out);
            if (external && type_is_pointer(arg->type)) {
                fputs("(void *)", out);
                emit_expr(arg, out);
            } else if (func->as.func.params[i].is_map) {
                /* The function is given the caller's own map. */
                fputs("&", out);
                emit_symbol(arg->as.ident.symbol, out);
            } else if (!is_array_name(arg)) {
                emit_expr(arg, out);
            } else if (func->as.func.params[i].is_growable) {
                /* The function is given the caller's own array, to grow. */
                fputs("&", out);
                emit_symbol(arg->as.ident.symbol, out);
            } else if (arg->as.ident.symbol->is_slice) {
                emit_symbol(arg->as.ident.symbol, out);
            } else if (arg->as.ident.symbol->is_growable) {
                /* A view of the elements the array holds now. */
                fputs("(", out);
                emit_slice_type(arg->as.ident.symbol->type, out);
                fputs("){ ", out);
                emit_symbol(arg->as.ident.symbol, out);
                fputs(".data, ", out);
                emit_symbol(arg->as.ident.symbol, out);
                fputs(".len }", out);
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
        if (expr->type == TYPE_STRING && is_array_name(expr->as.cast.operand) &&
            expr->as.cast.operand->as.ident.symbol->is_growable) {
            /* A view of the characters an array of char holds now. */
            const Symbol *chars = expr->as.cast.operand->as.ident.symbol;
            fprintf(out, "((%s){ (const char *)", c_type(TYPE_STRING));
            emit_symbol(chars, out);
            fputs(".data, ", out);
            emit_symbol(chars, out);
            fputs(".len })", out);
            break;
        }
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

static void emit_expr(const Expr *expr, FILE *out) {
    for (size_t i = held.count; i-- > 0;) {
        if (held.items[i] == expr) {
            fprintf(out, held.through[i] ? "(*wind_p%d)" : "wind_t%d", held.ids[i]);
            return;
        }
    }
    const Expr **operands;
    size_t count = unordered_operands(expr, &operands);
    if (count && order_matters(operands, count)) {
        size_t mark = hold_operands(operands, count, out);
        emit_expr_kind(expr, out);
        release_operands(mark, out);
    } else {
        emit_expr_kind(expr, out);
    }
    free(operands);
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
    if (is_format_call(arg)) {
        emit_format(arg, out);
        return;
    }
    const Symbol *map = map_named(arg);
    if (map) {
        /* A whole map prints as "{key: value, ...}", in the order its keys
           were first put. */
        used.helpers[HELPER_MAP] = true;
        used.helpers[HELPER_PUT] = true;
        int id = ++held.next_id;
        const char *key_cast;
        const char *value_cast;
        const char *key_writer = value_writer(map->key_type, true, &key_cast);
        const char *value_writer_name = value_writer(map->type, true, &value_cast);
        fprintf(out, "{ int wind_first%d = 1; wind_put(\"{\", 1); "
                     "for (size_t wind_i%d = 0; wind_i%d < ",
                id, id, id);
        emit_symbol(map, out);
        fprintf(out, ".used; wind_i%d++) { if (!", id);
        emit_symbol(map, out);
        fprintf(out, ".live[wind_i%d]) continue; if (!wind_first%d) wind_put(\", \", 2); "
                     "wind_first%d = 0; %s(%s((%s *)",
                id, id, id, key_writer, key_cast, c_type(map->key_type));
        emit_symbol(map, out);
        fprintf(out, ".keys)[wind_i%d]); wind_put(\": \", 2); %s(%s((%s *)", id,
                value_writer_name, value_cast, c_type(map->type));
        emit_symbol(map, out);
        fprintf(out, ".values)[wind_i%d]); } wind_put(\"}\", 1); }\n", id);
        return;
    }
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
        if (runtime_length(array)) {
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

/* The "m[key]" that a place is reached through, as in "m[key]" itself or
   "m[key].field"; NULL if it is not reached through a map. */
static const Expr *map_entry_of(const Expr *place) {
    while (place->kind == EXPR_FIELD && !place->as.field.through_pointer) {
        place = place->as.field.base;
    }
    return place->kind == EXPR_INDEX && map_named(place->as.index.base) ? place : NULL;
}

/* Writes into a map: the key, then the value, are worked out first, and
   then the entry is found or put there, and assigned. */
static void emit_map_assign(const Stmt *stmt, const Expr *entry, int depth, FILE *out) {
    const Symbol *map = map_named(entry->as.index.base);
    const Expr *target = stmt->as.assign.target;
    const Expr *value = stmt->as.assign.value;
    TypeKind type = target->type;
    Op op = stmt->as.assign.op;
    used.helpers[HELPER_MAP_PUT] = true;
    int id = ++held.next_id;
    size_t mark = held.count;
    indent(depth, out);
    fputs("{ ", out);
    emit_key_temp(map, entry->as.index.index, id, out);
    fprintf(out, "%s wind_t%d = ", c_type(value->type), id);
    emit_expr(value, out);
    fprintf(out, "; %s *wind_p%d = (%s *)wind_map_put(&", c_type(map->type), id,
            c_type(map->type));
    emit_symbol(map, out);
    fprintf(out, ", &wind_k%d, sizeof(%s), sizeof(%s), %d, %d); ", id, c_type(map->key_type),
            c_type(map->type), map->key_type == TYPE_STRING, stmt->loc.line);
    hold(entry, id, true);
    hold(value, id, false);
    emit_expr(target, out);
    fputs(" = ", out);
    if (!stmt->as.assign.compound) {
        emit_expr(value, out);
    } else if (op_is_shift(op)) {
        emit_shift_start(op, type, out);
        emit_expr(target, out);
        emit_shift_end(value, out);
    } else {
        fprintf(out, "((%s)(", c_type(type));
        emit_expr(target, out);
        fprintf(out, " %s ", op_text(op));
        emit_expr(value, out);
        fputs("))", out);
    }
    fputs("; }\n", out);
    held.count = mark;
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

/* True if a "continue" in the block, or in a block within it, goes on
   with loop. */
static bool continues(const Block *block, const Stmt *loop) {
    for (size_t i = 0; i < block->count; i++) {
        const Stmt *stmt = block->stmts[i];
        switch (stmt->kind) {
        case STMT_CONTINUE:
            if (stmt->as.target == loop) return true;
            break;
        case STMT_BLOCK:
            if (continues(&stmt->as.block, loop)) return true;
            break;
        case STMT_IF:
            if (continues(&stmt->as.if_.then_block, loop)) return true;
            if (stmt->as.if_.has_else && continues(&stmt->as.if_.else_block, loop)) {
                return true;
            }
            break;
        case STMT_WHILE:
        case STMT_FOR:
        case STMT_FOR_IN:
        case STMT_DO_WHILE:
            if (continues(&stmt->as.loop.body, loop)) return true;
            break;
        default:
            break;
        }
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

/* The deferred statements of the blocks being emitted, innermost last, and
   for each loop being emitted how many of them there were as it began. A
   block that is left runs its own, innermost first: at its end, and before
   a return, a break or a continue that leaves it. Each such place writes
   them out again. */
static struct {
    const Stmt **items;
    size_t count;
    size_t cap;
} deferred;

static struct {
    const Stmt **loops;
    size_t *marks;
    size_t count;
    size_t cap;
} loop_marks;

/* The function being emitted, whose return type a return needs; NULL in
   top-level code. */
static const Stmt *emitting_func;

/* Writes out the deferred statements above mark, innermost first. */
static void emit_deferred(size_t mark, int depth, FILE *out) {
    for (size_t i = deferred.count; i-- > mark;) {
        emit_stmt(deferred.items[i]->as.deferred, depth, out);
    }
}

static void push_loop(const Stmt *loop) {
    if (loop_marks.count == loop_marks.cap) {
        loop_marks.cap = loop_marks.cap ? loop_marks.cap * 2 : 8;
        loop_marks.loops = xrealloc(loop_marks.loops, loop_marks.cap * sizeof *loop_marks.loops);
        loop_marks.marks = xrealloc(loop_marks.marks, loop_marks.cap * sizeof *loop_marks.marks);
    }
    loop_marks.loops[loop_marks.count] = loop;
    loop_marks.marks[loop_marks.count++] = deferred.count;
}

/* How many deferred statements there were as the loop began. */
static size_t loop_mark(const Stmt *loop) {
    for (size_t i = loop_marks.count; i-- > 0;) {
        if (loop_marks.loops[i] == loop) return loop_marks.marks[i];
    }
    return deferred.count;
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
    case STMT_GROUP:
    case STMT_SETTING:
        break;
    case STMT_VAR:
        indent(depth, out);
        if (stmt->as.decl.is_map) {
            /* Empty, and then the entries given, in order. */
            const Symbol *map = stmt->as.decl.symbol;
            used.helpers[stmt->as.decl.elem_count ? HELPER_MAP_PUT : HELPER_MAP] = true;
            emit_symbol(map, out);
            fputs(" = (wind_map){0};\n", out);
            for (size_t i = 0; i + 1 < stmt->as.decl.elem_count; i += 2) {
                int id = ++held.next_id;
                indent(depth, out);
                fputs("{ ", out);
                emit_key_temp(map, stmt->as.decl.elems[i], id, out);
                fprintf(out, "%s wind_t%d = ", c_type(map->type), id);
                emit_expr(stmt->as.decl.elems[i + 1], out);
                fprintf(out, "; *(%s *)wind_map_put(&", c_type(map->type));
                emit_symbol(map, out);
                fprintf(out, ", &wind_k%d, sizeof(%s), sizeof(%s), %d, %d) = wind_t%d; }\n", id,
                        c_type(map->key_type), c_type(map->type),
                        map->key_type == TYPE_STRING, stmt->loc.line, id);
            }
            break;
        }
        if (stmt->as.decl.is_growable) {
            /* Empty, or a copy, and then the elements given, in order. */
            const Symbol *array = stmt->as.decl.symbol;
            emit_symbol(array, out);
            fputs(" = ", out);
            if (stmt->as.decl.init) {
                emit_clone(stmt->as.decl.init->as.call.args[0], array->type,
                           stmt->loc.line, out);
            } else {
                fputs("(", out);
                emit_list_type(array->type, out);
                fputs("){0}", out);
            }
            fputs(";\n", out);
            size_t count = stmt->as.decl.elem_count;
            if (count) {
                used.helpers[HELPER_GROW] = true;
                indent(depth, out);
                emit_symbol(array, out);
                fputs(".data = wind_grow(NULL, &", out);
                emit_symbol(array, out);
                fprintf(out, ".cap, %zu, sizeof(%s), %d);\n", count, c_type(array->type),
                        stmt->loc.line);
                for (size_t i = 0; i < count; i++) {
                    indent(depth, out);
                    emit_symbol(array, out);
                    fputs(".data[", out);
                    emit_symbol(array, out);
                    fputs(".len++] = ", out);
                    emit_expr(stmt->as.decl.elems[i], out);
                    fputs(";\n", out);
                }
            }
            break;
        }
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
        if (map_entry_of(target)) {
            emit_map_assign(stmt, map_entry_of(target), depth, out);
            break;
        }
        if (value->kind == EXPR_CALL && value->as.call.builtin == BUILTIN_CLONE) {
            /* The copy is made before the old elements are freed, since it
               may be a copy of them. */
            const Symbol *array = target->as.ident.symbol;
            used.headers |= HEADER_STDLIB;
            indent(depth, out);
            fputs("{ ", out);
            emit_list_type(array->type, out);
            fputs(" wind_copy = ", out);
            emit_clone(value->as.call.args[0], array->type, stmt->loc.line, out);
            fputs("; free(", out);
            emit_symbol(array, out);
            fputs(".data); ", out);
            emit_symbol(array, out);
            fputs(" = wind_copy; }\n", out);
            break;
        }
        TypeKind type = target->type;
        Op op = stmt->as.assign.op;
        bool compound = stmt->as.assign.compound;
        /* A target with an index or a pointer in it is written out once, so
           that what it computes is computed once. */
        bool once = !is_plain_place(target);
        /* The place is found, and read if the assignment is compound, before
           the value is evaluated. */
        bool ordered = compound ? (has_call(value) && !is_stable(target)) ||
                                      (has_call(target) && !is_stable(value))
                                : once && (has_call(value) ||
                                           (has_call(target) && !is_stable(value)));
        indent(depth, out);
        if (ordered || (compound && op_is_shift(op) && once)) {
            fprintf(out, "{ %s *wind_place = &", c_type(type));
            emit_expr(target, out);
            fputs("; ", out);
            if (!compound) {
                fputs("*wind_place = ", out);
                emit_expr(value, out);
                fputs("; }\n", out);
                break;
            }
            fprintf(out, "%s wind_old = *wind_place; *wind_place = ", c_type(type));
            if (op_is_shift(op)) {
                emit_shift_start(op, type, out);
                fputs("wind_old", out);
                emit_shift_end(value, out);
            } else {
                fprintf(out, "((%s)(wind_old %s ", c_type(type), op_text(op));
                emit_expr(value, out);
                fputs("))", out);
            }
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
        if (stmt->as.cout.to_error) {
            /* Written out whole, after what cout has written so far. */
            int id = ++held.next_id;
            used.helpers[HELPER_FLUSH] = true;
            used.helpers[HELPER_ERROR] = true;
            used.headers |= HEADER_STDLIB;
            indent(depth, out);
            fprintf(out, "{ wind_list_char wind_e%d = {0};\n", id);
            char into[32];
            snprintf(into, sizeof into, "&wind_e%d", id);
            indent(depth + 1, out);
            emit_written_into(stmt->as.cout.args, stmt->as.cout.count, into, stmt->loc.line,
                              true, out);
            fputs("\n", out);
            indent(depth + 1, out);
            fprintf(out, "wind_flush(); wind_error((const char *)wind_e%d.data); "
                         "free(wind_e%d.data); }\n",
                    id, id);
            break;
        }
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
        push_loop(stmt);
        emit_block(&stmt->as.loop.body, depth + 1, out);
        loop_marks.count--;
        if (stmt->as.loop.step) {
            if (continues(&stmt->as.loop.body, stmt)) {
                fprintf(out, "wind_step_%d:;\n", stmt->as.loop.id);
            }
            emit_stmt(stmt->as.loop.step, depth + 1, out);
        }
        indent(depth, out);
        fputs("}\n", out);
        break;
    case STMT_FOR_IN: {
        /* A C for loop, so that "continue" goes on to the next one. The
           length is read on each pass, since the body may change it. */
        const Expr *over = stmt->as.loop.over;
        const Param *each = stmt->as.loop.each;
        size_t count = stmt->as.loop.each_count;
        int id = ++held.next_id;
        const Symbol *map = map_named(over);
        const Symbol *array = !map && is_array_name(over) ? over->as.ident.symbol : NULL;
        indent(depth, out);
        fputs("{\n", out);
        if (!map && !array) {
            indent(depth + 1, out);
            fprintf(out, "%s wind_s%d = ", c_type(TYPE_STRING), id);
            emit_expr(over, out);
            fputs(";\n", out);
        }
        indent(depth + 1, out);
        fprintf(out, "for (size_t wind_i%d = 0; wind_i%d < ", id, id);
        if (map) {
            used.helpers[HELPER_MAP] = true;
            emit_symbol(map, out);
            fputs(".used", out);
        } else if (array && runtime_length(array)) {
            emit_symbol(array, out);
            fputs(".len", out);
        } else if (array) {
            fprintf(out, "%zu", array->length);
        } else {
            fprintf(out, "wind_s%d.len", id);
        }
        fprintf(out, "; wind_i%d++) {\n", id);
        if (map) {
            indent(depth + 2, out);
            fputs("if (!", out);
            emit_symbol(map, out);
            fprintf(out, ".live[wind_i%d]) continue;\n", id);
        }
        for (size_t i = 0; i < count; i++) {
            indent(depth + 2, out);
            emit_symbol(each[i].symbol, out);
            fputs(" = ", out);
            bool is_index = !map && count == 2 && i == 0;
            if (is_index) {
                fprintf(out, "(%s)wind_i%d;\n", c_type(TYPE_INT), id);
            } else if (map) {
                fprintf(out, "((%s *)", c_type(i == 0 ? map->key_type : map->type));
                emit_symbol(map, out);
                fprintf(out, ".%s)[wind_i%d];\n", i == 0 ? "keys" : "values", id);
            } else if (array) {
                emit_symbol(array, out);
                fprintf(out, "%s[wind_i%d];\n", runtime_length(array) ? ".data" : "", id);
            } else {
                fprintf(out, "(%s)wind_s%d.data[wind_i%d];\n", c_type(TYPE_CHAR), id, id);
            }
            /* A loop variable may be named only to reach the one after it. */
            indent(depth + 2, out);
            fputs("(void)", out);
            emit_symbol(each[i].symbol, out);
            fputs(";\n", out);
        }
        push_loop(stmt);
        emit_block(&stmt->as.loop.body, depth + 2, out);
        loop_marks.count--;
        indent(depth + 1, out);
        fputs("}\n", out);
        indent(depth, out);
        fputs("}\n", out);
        break;
    }
    case STMT_DO_WHILE:
        indent(depth, out);
        fputs("do {\n", out);
        push_loop(stmt);
        emit_block(&stmt->as.loop.body, depth + 1, out);
        loop_marks.count--;
        indent(depth, out);
        fputs("} while (", out);
        emit_expr(stmt->as.loop.cond, out);
        fputs(");\n", out);
        break;
    case STMT_DEFER:
        if (deferred.count == deferred.cap) {
            deferred.cap = deferred.cap ? deferred.cap * 2 : 8;
            deferred.items = xrealloc(deferred.items, deferred.cap * sizeof *deferred.items);
        }
        deferred.items[deferred.count++] = stmt;
        break;
    case STMT_BREAK:
        emit_deferred(loop_mark(stmt->as.target), depth, out);
        indent(depth, out);
        fputs("break;\n", out);
        break;
    case STMT_CONTINUE:
        emit_deferred(loop_mark(stmt->as.target), depth, out);
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
        if (deferred.count) {
            /* The value is worked out before the deferred statements run. */
            indent(depth, out);
            fputs("{\n", out);
            if (stmt->as.ret.value) {
                indent(depth + 1, out);
                fprintf(out, "%s wind_result = ", c_type(emitting_func->as.func.ret));
                emit_expr(stmt->as.ret.value, out);
                fputs(";\n", out);
            }
            emit_deferred(0, depth + 1, out);
            indent(depth + 1, out);
            fputs(stmt->as.ret.value ? "return wind_result;\n" : "return;\n", out);
            indent(depth, out);
            fputs("}\n", out);
            break;
        }
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
    size_t mark = deferred.count;
    for (size_t i = 0; i < block->count; i++) emit_stmt(block->stmts[i], depth, out);
    emit_deferred(mark, depth, out);
    deferred.count = mark;
}

static void emit_variables(const SymbolList *variables, const char *prefix, FILE *out) {
    for (size_t i = 0; i < variables->count; i++) {
        const Symbol *variable = variables->items[i];
        fputs(prefix, out);
        if (variable->is_growable) {
            emit_list_type(variable->type, out);
            fputs(" ", out);
        } else if (variable->is_map) {
            used.helpers[HELPER_MAP] = true;
            fputs("wind_map ", out);
        } else {
            fprintf(out, "%s ", c_type(variable->type));
        }
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
        } else if (param->is_growable) {
            emit_list_type(param->type, out);
            fputs(" *", out);
        } else if (param->is_map) {
            used.helpers[HELPER_MAP] = true;
            fputs("wind_map *", out);
        } else {
            fprintf(out, "%s ", c_type(param->type));
        }
        emit_symbol_name(param->symbol, out);
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
        if (!used.slices[type] && !used.lists[type]) continue;
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
        if (used.slices[type]) {
            fprintf(out, "typedef struct {\n    %s *data;\n    size_t len;\n} wind_slice_%s;\n",
                    type_c_name(type), type_name(type));
            any = true;
        }
        if (used.lists[type]) {
            fprintf(out,
                    "typedef struct {\n    %s *data;\n    size_t len;\n    size_t cap;\n"
                    "} wind_list_%s;\n",
                    type_c_name(type), type_name(type));
            any = true;
        }
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
        emitting_func = func;
        emit_block(&func->as.func.body, 1, out);
        emitting_func = NULL;
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
        if (info->slice_used) {
            used.headers |= HEADER_STDDEF;
            fprintf(out, "typedef struct {\n    %s *data;\n    size_t len;\n} wind_slice_%s;\n",
                    info->c_name, info->c_stem);
        }
        if (info->list_used) {
            used.headers |= HEADER_STDDEF;
            fprintf(out,
                    "typedef struct {\n    %s *data;\n    size_t len;\n    size_t cap;\n"
                    "} wind_list_%s;\n",
                    info->c_name, info->c_stem);
        }
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
    for (size_t i = 0; i < links.count; i++) free(links.items[i]);
    links.count = 0;
    types_use((TypeTable *)&unit->types);
    for (size_t i = 0; i < types_count(); i++) {
        TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        info->writer_used = info->writer_emitted = false;
        info->slice_used = info->list_used = info->array_writer_used = false;
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
