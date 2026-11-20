#define _POSIX_C_SOURCE 200809L /* open_memstream */

#include "codegen.h"
#include "targets/target.h"

#include <limits.h>
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

/* The file whose code is being emitted, whose text an assert quotes. */
static const Unit *emitting_unit;

/* What the program generated so far needs from the runtime. */
static struct {
    bool types[TYPE_COUNT];
    /* Views and growable arrays of built-in types, by element type, which
       the runtime defines since its helpers use some of them. */
    bool slices[TYPE_COUNT];
    bool lists[TYPE_COUNT];
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
    /* A declared type or a pointer type is always defined, and an array or
       a map once it is named; see emit_declared_types. */
    TypeInfo *info = type_info(type);
    if (type < TYPE_COUNT) used.types[type] = true;
    if (info && info->container) info->needed = true;
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
        "static void *wind_deref(const void *p, int line) {\n"
        "    if (!p) {\n"
        "        wind_flush();\n"
        "        wind_error(\"wind: line \");\n"
        "        wind_error_number(line, 1);\n"
        "        wind_error(\": a NULL pointer was used\\n\");\n"
        "        wind_stop();\n"
        "    }\n"
        "    return (void *)p;\n"
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
    [HELPER_STRING_COMPARE] = {
        "/* Below zero if a comes before b, byte by byte, zero if they are the\n"
        "   same, and above zero if a comes after: a string comes before any\n"
        "   longer one that starts with it. */\n"
        "static int wind_string_compare(wind_string a, wind_string b) {\n"
        "    size_t n = a.len < b.len ? a.len : b.len;\n"
        "    int order = n ? memcmp(a.data, b.data, n) : 0;\n"
        "    if (order) return order;\n"
        "    return (a.len > b.len) - (a.len < b.len);\n"
        "}\n",
        HEADER_STRING, {0}, {TYPE_STRING}},
    [HELPER_SORT] = {
        "/* Sorts count elements of size bytes at data into the order that order\n"
        "   gives the keys found offset bytes into them, keeping equal ones in\n"
        "   the order they were in: a merge sort, through a scratch copy. */\n"
        "static void wind_sort(void *data, size_t count, size_t size, size_t offset,\n"
        "                      int (*order)(const void *, const void *), int line) {\n"
        "    if (count < 2) return;\n"
        "    char *from = data;\n"
        "    char *into = malloc(count * size);\n"
        "    if (!into) wind_out_of_memory(line);\n"
        "    char *scratch = into;\n"
        "    for (size_t width = 1; width < count; width *= 2) {\n"
        "        for (size_t lo = 0; lo < count; lo += 2 * width) {\n"
        "            size_t mid = lo + width < count ? lo + width : count;\n"
        "            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;\n"
        "            size_t i = lo, j = mid, k = lo;\n"
        "            while (i < mid && j < hi) {\n"
        "                if (order(from + j * size + offset, from + i * size + offset) < 0) {\n"
        "                    memcpy(into + k++ * size, from + j++ * size, size);\n"
        "                } else {\n"
        "                    memcpy(into + k++ * size, from + i++ * size, size);\n"
        "                }\n"
        "            }\n"
        "            memcpy(into + k * size, from + i * size, (mid - i) * size);\n"
        "            k += mid - i;\n"
        "            memcpy(into + k * size, from + j * size, (hi - j) * size);\n"
        "        }\n"
        "        char *swap = from;\n"
        "        from = into;\n"
        "        into = swap;\n"
        "    }\n"
        "    if (from != (char *)data) memcpy(data, from, count * size);\n"
        "    free(scratch);\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB | HEADER_STRING, {HELPER_OUT_OF_MEMORY}, {0}},
    [HELPER_REVERSE] = {
        "/* Puts count elements of size bytes at data into the opposite order. */\n"
        "static void wind_reverse(void *data, size_t count, size_t size) {\n"
        "    unsigned char *items = data;\n"
        "    for (size_t i = 0; i < count / 2; i++) {\n"
        "        unsigned char *a = items + i * size;\n"
        "        unsigned char *b = items + (count - 1 - i) * size;\n"
        "        for (size_t byte = 0; byte < size; byte++) {\n"
        "            unsigned char t = a[byte];\n"
        "            a[byte] = b[byte];\n"
        "            b[byte] = t;\n"
        "        }\n"
        "    }\n"
        "}\n",
        HEADER_STDDEF, {0}, {0}},
    [HELPER_SCRATCH] = {
        "/* Scratch memory: the text that \"+\" on strings makes, kept until the\n"
        "   #temp block it was made in ends, or the program does. */\n"
        "static struct {\n"
        "    void **blocks;\n"
        "    size_t count;\n"
        "    size_t cap;\n"
        "} wind_scratch;\n"
        "\n"
        "/* Keeps text in scratch memory, and gives a view of it. */\n"
        "static wind_string wind_keep(wind_list_char *text, int line) {\n"
        "    if (wind_scratch.count == wind_scratch.cap) {\n"
        "        size_t cap = wind_scratch.cap ? wind_scratch.cap * 2 : 64;\n"
        "        void **grown = realloc(wind_scratch.blocks, cap * sizeof *grown);\n"
        "        if (!grown) wind_out_of_memory(line);\n"
        "        wind_scratch.blocks = grown;\n"
        "        wind_scratch.cap = cap;\n"
        "    }\n"
        "    wind_scratch.blocks[wind_scratch.count++] = text->data;\n"
        "    wind_string kept = { (const char *)text->data, text->len };\n"
        "    return kept;\n"
        "}\n"
        "\n"
        "static size_t wind_scratch_mark(void) {\n"
        "    return wind_scratch.count;\n"
        "}\n"
        "\n"
        "/* Frees the scratch memory kept since mark. */\n"
        "static void wind_scratch_free(size_t mark) {\n"
        "    while (wind_scratch.count > mark) free(wind_scratch.blocks[--wind_scratch.count]);\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDLIB, {HELPER_OUT_OF_MEMORY}, {TYPE_STRING, TYPE_CHAR}},
    [HELPER_DIV_FAIL] = {
        "WIND_RARE static _Noreturn void wind_div_fail(int line) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": division by zero\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_FLOAT_FAIL] = {
        "/* Reports a float converted to an integer type that cannot hold it. */\n"
        "WIND_RARE static _Noreturn void wind_float_fail(int line, const char *type) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": a float that does not fit in '\");\n"
        "    wind_error(type);\n"
        "    wind_error(\"' was converted to it\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_ASSERT_FAIL] = {
        "WIND_RARE static _Noreturn void wind_assert_fail(int line, const char *condition) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": assertion failed: \");\n"
        "    wind_error(condition);\n"
        "    wind_error(\"\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {0}},
    [HELPER_PANIC] = {
        "/* Stops the program with message, which ends with a '\\0'. */\n"
        "WIND_RARE static _Noreturn void wind_panic(int line, const wind_char *message) {\n"
        "    wind_flush();\n"
        "    wind_error(\"wind: line \");\n"
        "    wind_error_number(line, 1);\n"
        "    wind_error(\": \");\n"
        "    wind_error((const char *)message);\n"
        "    wind_error(\"\\n\");\n"
        "    wind_stop();\n"
        "}\n",
        0, {HELPER_HINTS, HELPER_FLUSH, HELPER_ERROR_NUMBER, HELPER_STOP}, {TYPE_CHAR}},
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
/* The headers that libraries carry and the program uses, each once. */
static struct {
    Str *items;
    size_t count;
    size_t cap;
} carried;

static void use_external(const Stmt *decl) {
    use_link(decl->link);
    if (decl->carried.len) {
        for (size_t i = 0; i < carried.count; i++) {
            if (str_eq(carried.items[i], decl->carried)) return;
        }
        if (carried.count == carried.cap) {
            carried.cap = carried.cap ? carried.cap * 2 : 4;
            carried.items = xrealloc(carried.items, carried.cap * sizeof *carried.items);
        }
        carried.items[carried.count++] = decl->carried;
        return;
    }
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
    ContainerKind kind = type_container(symbol->type);
    bool through = symbol->kind == SYM_PARAM && (kind == CONTAINER_LIST || kind == CONTAINER_MAP);
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
    fputs(c_type(type_slice_of(elem)), out);
}

/* The type of a growable array of an element type, noted as needing its
   definition. Its data and len come first, as in a slice. */
static void emit_list_type(TypeKind elem, FILE *out) {
    fputs(c_type(type_list_of(elem)), out);
}

/* True for a constant that is stored in the generated program, and so is
   referred to by name; other constants are written out where they are used. */
static bool is_stored_const(const Symbol *symbol) {
    return symbol->kind == SYM_CONST && (type_is_container(symbol->type) ||
                                         symbol->type == TYPE_STRING ||
                                         type_is_struct(symbol->type));
}

static void emit_field_name(Str name, FILE *out) {
    fprintf(out, "f_%.*s", (int)name.len, name.data);
}

/* Emits the place that a read stores into. */
static void emit_place(const Symbol *symbol, const Expr *index, const PlaceField *fields,
                       size_t field_count, FILE *out);

/* True for an expression that stands for a whole array or map, rather than
   a pointer to its first element. */
static bool is_whole(const Expr *expr) {
    return type_is_container(expr->type) && !expr->decayed;
}

/* Writes a whole array or map, as an lvalue. */
static void emit_whole(const Expr *expr, FILE *out) {
    fputs("(", out);
    emit_expr(expr, out);
    fputs(")", out);
}

/* True for a whole array or map that costs nothing to write out twice: a
   variable, or a field of one or of what a pointer in one points to. */
static bool is_plain(const Expr *expr) {
    while (expr->kind == EXPR_FIELD) expr = expr->as.field.base;
    return expr->kind == EXPR_IDENT;
}

/* The member of an array's C form that holds its elements. */
static const char *elements_of(TypeKind type) {
    return runtime_length(type) ? ".data" : ".v";
}

static void hold(const Expr *expr, int id, bool through);

static void emit_checked_pointer(const Expr *pointer, FILE *out);


static void emit_element(const Expr *base, const Expr *index, FILE *out);


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

/* One element of an array, as something that can be read or assigned. An
   array that takes work to reach is reached once, through a pointer. */
static void emit_element(const Expr *base, const Expr *index, FILE *out) {
    TypeKind type = base->type;
    if (!runtime_length(type) && index->kind == EXPR_INT_LIT &&
        index->as.int_lit < type_length(type)) {
        /* Needs nothing worked out when the program runs. */
        emit_whole(base, out);
        fprintf(out, ".v[%zu]", (size_t)index->as.int_lit);
        return;
    }
    bool held_already = false;
    for (size_t i = 0; i < held.count; i++) held_already = held_already || held.items[i] == base;
    if (!is_plain(base) && !held_already) {
        int id = ++held.next_id;
        size_t mark = held.count;
        fprintf(out, "(*__extension__ ({ %s *wind_p%d = &", c_type(type), id);
        emit_whole(base, out);
        fputs("; &", out);
        hold(base, id, true);
        emit_element(base, index, out);
        held.count = mark;
        fputs("; }))", out);
        return;
    }
    emit_whole(base, out);
    fputs(elements_of(type), out);
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
    if (runtime_length(type)) {
        emit_whole(base, out);
        fputs(".len", out);
    } else {
        fprintf(out, "%zu", type_length(type));
    }
    fprintf(out, ", %d)]", index->loc.line);
}

/* Emits a pointer, checked for NULL, as a pointer of its own type again. */
static void emit_checked_pointer(const Expr *pointer, FILE *out) {
    used.helpers[HELPER_DEREF] = true;
    if (type_is_function(pointer->type)) {
        /* Kept apart from data pointers, which C does not convert it to. */
        int id = ++held.next_id;
        fprintf(out, "__extension__ ({ %s wind_f%d = ", c_type(pointer->type), id);
        emit_expr(pointer, out);
        fprintf(out, "; if (!wind_f%d) wind_deref(0, %d); wind_f%d; })", id, pointer->loc.line, id);
        return;
    }
    fprintf(out, "((%s)wind_deref(", c_type(pointer->type));
    emit_expr(pointer, out);
    fprintf(out, ", %d))", pointer->loc.line);
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
        if (type_is_container(symbol->type)) return true;
        return !symbol->global && !symbol->address_taken;
    }
    case EXPR_LEN:
        /* A growable array's or a map's length changes as it grows. */
        if (is_whole(expr->as.len.operand)) {
            ContainerKind kind = type_container(expr->as.len.operand->type);
            return kind == CONTAINER_ARRAY ||
                   (kind == CONTAINER_SLICE && is_stable(expr->as.len.operand));
        }
        return is_stable(expr->as.len.operand);
    case EXPR_UNARY:
        if (expr->as.unary.op == OP_ADDR) return expr->as.unary.operand->kind == EXPR_IDENT;
        return expr->as.unary.op != OP_DEREF && is_stable(expr->as.unary.operand);
    case EXPR_BINARY:
        return is_stable(expr->as.binary.left) && is_stable(expr->as.binary.right);
    case EXPR_CAST:
        /* A view of a growable array's text changes with it. */
        if (expr->type == TYPE_STRING && is_whole(expr->as.cast.operand)) return false;
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
        /* A pointer to a function is found before its arguments are. */
        *operands = xmalloc((expr->as.call.count + 1) * sizeof **operands);
        if (expr->as.call.callee) (*operands)[count++] = expr->as.call.callee;
        for (size_t i = 0; i < expr->as.call.count; i++) {
            (*operands)[count++] = expr->as.call.args[i];
        }
        break;
    case EXPR_INIT:
        count = expr->as.init.count;
        *operands = xmalloc((count ? count : 1) * sizeof **operands);
        for (size_t i = 0; i < count; i++) (*operands)[i] = expr->as.init.items[i].value;
        break;
    case EXPR_BINARY:
        if (expr->as.binary.op == OP_AND || expr->as.binary.op == OP_OR) break;
        /* Joined strings work out their parts in order themselves. */
        if (expr->as.binary.op == OP_ADD && expr->type == TYPE_STRING) break;
        count = 2;
        *operands = xmalloc(2 * sizeof **operands);
        (*operands)[0] = expr->as.binary.left;
        (*operands)[1] = expr->as.binary.right;
        break;
    case EXPR_INDEX:
        if (is_whole(expr->as.index.base)) break;
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


static void emit_joined(const Expr *expr, FILE *out);

/* True for a divisor that is a literal other than zero, which needs no
   test before dividing by it. */
static bool nonzero_literal(const Expr *divisor) {
    if (divisor->kind == EXPR_UNARY && divisor->as.unary.op == OP_NEG) {
        divisor = divisor->as.unary.operand;
    }
    return divisor->kind == EXPR_INT_LIT && divisor->as.int_lit != 0;
}

/* True for an integer division or remainder whose divisor is tested before
   it is used: in checked code, unless it is a literal other than zero. */
static bool checked_division(Op op, TypeKind type, const Expr *divisor) {
    return (op == OP_DIV || op == OP_MOD) && type_is_integer(type) && bounds_checked &&
           !nonzero_literal(divisor);
}

/* "left op right" for an integer division or remainder, with left already
   written as the C expression in left_text when it is not NULL: a divisor
   of zero stops the program, and one of -1 gives the wrapped result, as C's
   own division by it can stop the program for the most negative value. */
static void emit_division(Op op, TypeKind type, const Expr *left, const char *left_text,
                          const Expr *right, int line, FILE *out) {
    used.helpers[HELPER_DIV_FAIL] = true;
    int id = ++held.next_id;
    const char *t = c_type(type);
    fprintf(out, "__extension__ ({ %s wind_l%d = ", t, id);
    if (left_text) {
        fputs(left_text, out);
    } else {
        emit_expr(left, out);
    }
    fprintf(out, "; %s wind_r%d = ", t, id);
    emit_expr(right, out);
    fprintf(out, "; if (WIND_UNLIKELY(wind_r%d == 0)) wind_div_fail(%d); ", id, line);
    if (type_is_signed(type) && op == OP_DIV) {
        fprintf(out, "wind_r%d == -1 ? (%s)(0 - wind_l%d) : (%s)(wind_l%d / wind_r%d); })", id, t,
                id, t, id, id);
    } else if (type_is_signed(type)) {
        fprintf(out, "wind_r%d == -1 ? (%s)0 : (%s)(wind_l%d %% wind_r%d); })", id, t, t, id, id);
    } else {
        fprintf(out, "(%s)(wind_l%d %s wind_r%d); })", t, id, op_text(op), id);
    }
}

static void emit_binary(const Expr *expr, FILE *out) {
    Op op = expr->as.binary.op;
    const Expr *left = expr->as.binary.left;
    const Expr *right = expr->as.binary.right;
    if (checked_division(op, expr->type, right)) {
        emit_division(op, expr->type, left, NULL, right, expr->loc.line, out);
        return;
    }
    if (op == OP_ADD && expr->type == TYPE_STRING) {
        emit_joined(expr, out);
        return;
    }

    if (left->type == TYPE_STRING && op != OP_EQ && op != OP_NE) {
        used.helpers[HELPER_STRING_COMPARE] = true;
        fputs("(wind_string_compare(", out);
        emit_expr(left, out);
        fputs(", ", out);
        emit_expr(right, out);
        fprintf(out, ") %s 0)", op_text(op));
        return;
    }
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

/* "data, count" for a whole array. */
static void emit_array_extent(const Expr *expr, FILE *out) {
    emit_whole(expr, out);
    fputs(elements_of(expr->type), out);
    if (runtime_length(expr->type)) {
        fputs(", ", out);
        emit_whole(expr, out);
        fputs(".len", out);
    } else {
        fprintf(out, ", %zu", type_length(expr->type));
    }
}

/* A new growable array of elem holding a copy of source's elements, or of
   a string's bytes. */
static void emit_clone(const Expr *source, TypeKind elem, int line, FILE *out) {
    used.helpers[HELPER_CLONE] = true;
    int id = ++held.next_id;
    if (!is_whole(source)) {
        fprintf(out, "__extension__ ({ %s wind_s%d = ", c_type(TYPE_STRING), id);
        emit_expr(source, out);
        fputs("; (", out);
        emit_list_type(elem, out);
        fprintf(out, "){ wind_clone(wind_s%d.data, wind_s%d.len, 1, %d), wind_s%d.len, "
                     "wind_s%d.len }; })",
                id, id, line, id, id);
        return;
    }
    fprintf(out, "__extension__ ({ %s *wind_s%d = &", c_type(source->type), id);
    emit_whole(source, out);
    fprintf(out, "; size_t wind_n%d = ", id);
    if (runtime_length(source->type)) {
        fprintf(out, "wind_s%d->len", id);
    } else {
        fprintf(out, "%zu", type_length(source->type));
    }
    fputs("; (", out);
    emit_list_type(elem, out);
    fprintf(out, "){ wind_clone(wind_s%d->%s, wind_n%d, sizeof(%s), %d), wind_n%d, wind_n%d }; })",
            id, runtime_length(source->type) ? "data" : "v", id, c_type(elem), line, id, id);
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
    if (is_whole(arg)) return;
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

/* Collects the parts that a chain of "+" on strings joins, in order. */
static void collect_joined(const Expr *expr, const Expr ***parts, size_t *count, size_t *cap) {
    if (expr->kind == EXPR_BINARY && expr->as.binary.op == OP_ADD && expr->type == TYPE_STRING) {
        collect_joined(expr->as.binary.left, parts, count, cap);
        collect_joined(expr->as.binary.right, parts, count, cap);
        return;
    }
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *parts = xrealloc(*parts, *cap * sizeof **parts);
    }
    (*parts)[(*count)++] = expr;
}

/* Strings joined with "+": every part is worked out, then written as text
   into a new array of char, which is kept in scratch memory. */
static void emit_joined(const Expr *expr, FILE *out) {
    const Expr **parts = NULL;
    size_t count = 0, cap = 0;
    collect_joined(expr, &parts, &count, &cap);
    used.helpers[HELPER_SCRATCH] = true;
    int id = ++held.next_id;
    fprintf(out, "__extension__ ({ wind_list_char wind_j%d = {0}; ", id);
    char into[32];
    snprintf(into, sizeof into, "&wind_j%d", id);
    emit_written_into((Expr *const *)parts, count, into, expr->loc.line, false, out);
    fprintf(out, " wind_keep(&wind_j%d, %d); })", id, expr->loc.line);
    free(parts);
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

/* The types that sort compares, each of which needs its comparison. */
static struct {
    TypeKind items[64];
    size_t count;
} orders;

static void emit_order_name(TypeKind type, FILE *out) {
    const TypeInfo *info = type_info(type);
    fprintf(out, "wind_order_%s", info ? info->c_stem : type_name(type));
}


/* sort and reverse. */
static void emit_order_builtin(const Expr *expr, FILE *out) {
    const Expr *first = expr->as.call.args[0];
    TypeKind elem = type_elem(first->type);
    if (expr->as.call.builtin == BUILTIN_REVERSE) {
        used.helpers[HELPER_REVERSE] = true;
        fputs("wind_reverse(", out);
        emit_array_extent(first, out);
        fprintf(out, ", sizeof(%s))", c_type(elem));
        return;
    }
    TypeKind key = elem;
    const TypeInfo *info = type_info(elem);
    const Expr *field = expr->as.call.count == 2 ? expr->as.call.args[1] : NULL;
    if (field) key = info->fields[field->as.key.index].type;
    bool known = false;
    for (size_t i = 0; i < orders.count; i++) known = known || orders.items[i] == key;
    if (!known && orders.count < sizeof orders.items / sizeof orders.items[0]) {
        orders.items[orders.count++] = key;
    }
    if (key == TYPE_STRING) used.helpers[HELPER_STRING_COMPARE] = true;
    used.helpers[HELPER_SORT] = true;
    fputs("wind_sort(", out);
    emit_array_extent(first, out);
    fprintf(out, ", sizeof(%s), ", c_type(elem));
    if (field) {
        fprintf(out, "offsetof(%s, ", c_type(elem));
        emit_field_name(info->fields[field->as.key.index].name, out);
        fputs(")", out);
    } else {
        fputs("0", out);
    }
    fputs(", ", out);
    emit_order_name(key, out);
    fprintf(out, ", %d)", expr->loc.line);
}

/* The comparison of each type sort was used on. */
static void emit_orders(FILE *out) {
    for (size_t i = 0; i < orders.count; i++) {
        TypeKind type = orders.items[i];
        fputs("static int ", out);
        emit_order_name(type, out);
        fputs("(const void *a, const void *b) {\n", out);
        if (type == TYPE_STRING) {
            fprintf(out, "    return wind_string_compare(*(const %s *)a, *(const %s *)b);\n",
                    c_type(type), c_type(type));
        } else {
            fprintf(out, "    %s x = *(const %s *)a, y = *(const %s *)b;\n"
                         "    return (x > y) - (x < y);\n",
                    c_type(type), c_type(type), c_type(type));
        }
        fputs("}\n\n", out);
    }
}

/* True for an expression that stands for a whole map. */
static bool is_map(const Expr *expr) {
    return is_whole(expr) && type_container(expr->type) == CONTAINER_MAP;
}

/* "sizeof(K), is_string", what the map functions are told of a key. */
static void emit_key_facts(TypeKind map, FILE *out) {
    fprintf(out, "sizeof(%s), %d", c_type(type_key(map)), type_key(map) == TYPE_STRING);
}

/* Declares "K wind_kN = key; " for a map's key. */
static void emit_key_temp(TypeKind map, const Expr *key, int id, FILE *out) {
    fprintf(out, "%s wind_k%d = ", c_type(type_key(map)), id);
    emit_expr(key, out);
    fputs("; ", out);
}

/* Looking up a key. Read, it is the value stored under it, or zero; one
   that is an array or a map is an lvalue, the zero one a value of its own
   that nothing changes. Changed, the key is put in first if it is
   missing, with a value of zero. */
static void emit_map_index(const Expr *expr, FILE *out) {
    const Expr *base = expr->as.index.base;
    TypeKind map = base->type;
    TypeKind value = type_elem(map);
    int id = ++held.next_id;
    if (expr->as.index.inserts) {
        used.helpers[HELPER_MAP_PUT] = true;
        fprintf(out, "(*(%s *)__extension__ ({ wind_map *wind_m%d = &", c_type(value), id);
        emit_whole(base, out);
        fputs("; ", out);
        emit_key_temp(map, expr->as.index.index, id, out);
        fprintf(out, "wind_map_put(wind_m%d, &wind_k%d, sizeof(%s), sizeof(%s), %d, %d); }))", id,
                id, c_type(type_key(map)), c_type(value), type_key(map) == TYPE_STRING,
                expr->loc.line);
        return;
    }
    used.helpers[HELPER_MAP_FIND] = true;
    bool whole = type_is_container(value);
    fprintf(out, "%s__extension__ ({ wind_map *wind_m%d = &", whole ? "(*" : "", id);
    emit_whole(base, out);
    fputs("; ", out);
    emit_key_temp(map, expr->as.index.index, id, out);
    fprintf(out, "size_t wind_i%d = wind_map_find(wind_m%d, &wind_k%d, ", id, id, id);
    emit_key_facts(map, out);
    if (whole) {
        fprintf(out, "); static %s wind_z%d; wind_i%d == (size_t)-1 ? &wind_z%d : "
                     "&((%s *)wind_m%d->values)[wind_i%d]; }))",
                c_type(value), id, id, id, c_type(value), id, id);
    } else {
        fprintf(out, "); wind_i%d == (size_t)-1 ? (%s){0} : ((%s *)wind_m%d->values)[wind_i%d]; })",
                id, c_type(value), c_type(value), id, id);
    }
}

/* has, remove, clear and delete on a map. */
static void emit_map_builtin(const Expr *expr, FILE *out) {
    int id = ++held.next_id;
    Builtin which = expr->as.call.builtin;
    const Expr *base = expr->as.call.args[0];
    TypeKind map = base->type;
    used.helpers[which == BUILTIN_HAS      ? HELPER_MAP_FIND
                 : which == BUILTIN_REMOVE ? HELPER_MAP_REMOVE
                 : which == BUILTIN_CLEAR  ? HELPER_MAP_CLEAR
                                           : HELPER_MAP_DELETE] = true;
    fprintf(out, "__extension__ ({ wind_map *wind_m%d = &", id);
    emit_whole(base, out);
    fputs("; ", out);
    if (which == BUILTIN_HAS || which == BUILTIN_REMOVE) {
        emit_key_temp(map, expr->as.call.args[1], id, out);
        if (which == BUILTIN_HAS) {
            fprintf(out, "(%s)(wind_map_find(wind_m%d", c_type(TYPE_BOOL), id);
        } else {
            fprintf(out, "wind_map_remove(wind_m%d", id);
        }
        fprintf(out, ", &wind_k%d, ", id);
        emit_key_facts(map, out);
        fputs(which == BUILTIN_HAS ? ") != (size_t)-1); })" : "); (void)0; })", out);
        return;
    }
    fprintf(out, which == BUILTIN_CLEAR ? "wind_map_clear(wind_m%d" : "wind_map_delete(wind_m%d",
            id);
    fputs(", ", out);
    emit_key_facts(map, out);
    fputs("); (void)0; })", out);
}

/* A call of one of the built-in functions on growable arrays. Those that
   give no value are statement expressions ending in "(void)0". */
static void emit_builtin(const Expr *expr, FILE *out) {
    if (expr->as.call.builtin == BUILTIN_ASSERT) {
        /* The condition, as written, is the message. */
        const Expr *condition = expr->as.call.args[0];
        used.helpers[HELPER_ASSERT_FAIL] = true;
        fputs("((", out);
        emit_expr(condition, out);
        fprintf(out, ") ? (void)0 : wind_assert_fail(%d, ", expr->loc.line);
        Str text = {"(condition)", 11};
        if (emitting_unit && condition->loc.offset + condition->loc.len <= emitting_unit->src.len) {
            text = (Str){emitting_unit->src.data + condition->loc.offset, condition->loc.len};
        }
        emit_c_string(text, out);
        fputs("))", out);
        return;
    }
    if (expr->as.call.builtin == BUILTIN_PANIC) {
        /* The message is written as cerr would write it. */
        used.helpers[HELPER_PANIC] = true;
        int id = ++held.next_id;
        fprintf(out, "__extension__ ({ wind_list_char wind_e%d = {0}; ", id);
        char into[32];
        snprintf(into, sizeof into, "&wind_e%d", id);
        emit_written_into(expr->as.call.args, expr->as.call.count, into, expr->loc.line, true, out);
        fprintf(out, " wind_panic(%d, wind_e%d.data); })", expr->loc.line, id);
        return;
    }
    if (expr->as.call.builtin == BUILTIN_SORT || expr->as.call.builtin == BUILTIN_REVERSE) {
        emit_order_builtin(expr, out);
        return;
    }
    if (expr->as.call.builtin == BUILTIN_CLONE) {
        emit_clone(expr->as.call.args[0], type_elem(expr->type), expr->loc.line, out);
        return;
    }
    if (expr->as.call.builtin == BUILTIN_EXIT) {
        used.helpers[HELPER_EXIT] = true;
        fputs("wind_exit((int)(", out);
        emit_expr(expr->as.call.args[0], out);
        fputs("))", out);
        return;
    }
    if (is_map(expr->as.call.args[0])) {
        emit_map_builtin(expr, out);
        return;
    }
    const Expr *first = expr->as.call.args[0];
    TypeKind elem = type_elem(first->type);
    int line = expr->loc.line;
    int id = ++held.next_id;
    if (expr->as.call.builtin == BUILTIN_POP) {
        /* An lvalue, which an element that is an array needs. */
        fputs("(*", out);
    }
    fputs("__extension__ ({ ", out);
    if (expr->as.call.builtin == BUILTIN_APPEND && elem == TYPE_CHAR) {
        /* Into an array of char, each value is written as text. */
        use_sink();
        fprintf(out, "wind_list_char *wind_in%d = &", id);
        emit_whole(first, out);
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
            fprintf(out, "%s wind_a%d_%zu = ", c_type(elem), id, i);
            emit_expr(expr->as.call.args[i + 1], out);
            fputs("; ", out);
        }
        used.helpers[HELPER_GROW] = true;
        emit_list_type(elem, out);
        fprintf(out, " *wind_d%d = &", id);
        emit_whole(first, out);
        fprintf(out, "; wind_d%d->data = wind_grow(wind_d%d->data, &wind_d%d->cap, "
                     "wind_d%d->len + %zu, sizeof *wind_d%d->data, %d); ",
                id, id, id, id, count, id, line);
        for (size_t i = 0; i < count; i++) {
            fprintf(out, "wind_d%d->data[wind_d%d->len++] = wind_a%d_%zu; ", id, id, id, i);
        }
        fputs("(void)0; })", out);
        return;
    }
    emit_list_type(elem, out);
    fprintf(out, " *wind_d%d = &", id);
    emit_whole(first, out);
    fputs("; ", out);
    switch (expr->as.call.builtin) {
    case BUILTIN_POP:
        if (bounds_checked) {
            used.helpers[HELPER_POP_CHECK] = true;
            fprintf(out, "wind_pop_check(wind_d%d->len, %d); ", id, line);
        }
        fprintf(out, "&wind_d%d->data[--wind_d%d->len]; }))", id, id);
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
        if (is_map(base)) {
            emit_map_index(expr, out);
            break;
        }
        if (is_whole(base)) {
            emit_element(base, expr->as.index.index, out);
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
                               (operand->kind == EXPR_IDENT && !is_whole(operand) &&
                                operand->as.ident.symbol->kind == SYM_CONST);
        if (is_const_string) {
            /* Written as a number so that it can sit in a C initializer. */
            fprintf(out, "%zu)", const_string(operand).len);
        } else if (type_container(operand->type) == CONTAINER_ARRAY) {
            fprintf(out, "%zu)", type_length(operand->type));
        } else {
            fputs("(", out);
            emit_expr(operand, out);
            fputs(").len)", out);
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
        ContainerKind kind = type_container(expr->type);
        if (kind == CONTAINER_ARRAY) {
            /* Elements left out are zero, as in C. */
            fprintf(out, "((%s){ { ", c_type(expr->type));
            for (size_t i = 0; i < expr->as.init.count; i++) {
                if (i) fputs(", ", out);
                emit_expr(expr->as.init.items[i].value, out);
            }
            if (expr->as.init.count == 0) fputs("0", out);
            fputs(" } })", out);
            break;
        }
        if (kind == CONTAINER_LIST) {
            size_t count = expr->as.init.count;
            if (count == 0) {
                fprintf(out, "((%s){0})", c_type(expr->type));
                break;
            }
            TypeKind elem = type_elem(expr->type);
            int id = ++held.next_id;
            used.helpers[HELPER_CLONE] = true;
            fprintf(out, "__extension__ ({ %s wind_e%d[] = { ", c_type(elem), id);
            for (size_t i = 0; i < count; i++) {
                if (i) fputs(", ", out);
                emit_expr(expr->as.init.items[i].value, out);
            }
            fprintf(out, " }; (%s){ wind_clone(wind_e%d, %zu, sizeof(%s), %d), %zu, %zu }; })",
                    c_type(expr->type), id, count, c_type(elem), expr->loc.line, count, count);
            break;
        }
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
        const Expr *callee = expr->as.call.callee;
        const Stmt *func = callee ? NULL : expr->as.call.symbol->decl;
        const TypeInfo *pointer = callee ? type_info(callee->type) : NULL;
        bool external = func && func->external && !func->carried.len;
        bool cast_result = external && type_is_pointer(expr->type);
        if (cast_result) fprintf(out, "((%s)(void *)", c_type(expr->type));
        if (!callee) {
            emit_symbol(expr->as.call.symbol, out);
        } else if (bounds_checked) {
            /* Calling through NULL stops the program with the line. */
            emit_checked_pointer(callee, out);
        } else {
            emit_whole(callee, out);
        }
        fputs("(", out);
        for (size_t i = 0; i < expr->as.call.count; i++) {
            const Expr *arg = expr->as.call.args[i];
            if (i) fputs(", ", out);
            if (func && i >= func->as.func.param_count) {
                /* After "...": as C takes it, pointers as "void *". */
                if (type_is_pointer(arg->type)) fputs("(void *)", out);
                emit_expr(arg, out);
                continue;
            }
            TypeKind param = callee ? pointer->params[i] : func->as.func.params[i].type;
            if (external && type_is_pointer(arg->type)) {
                fputs("(void *)", out);
                emit_expr(arg, out);
            } else if (!type_is_container(param)) {
                emit_expr(arg, out);
            } else if (type_container(param) != CONTAINER_SLICE) {
                /* The function is given the caller's own growable array or
                   map, to change. */
                fputs("&", out);
                emit_whole(arg, out);
            } else if (type_container(arg->type) == CONTAINER_SLICE) {
                emit_whole(arg, out);
            } else {
                /* A view of the elements an array holds now. A constant one
                   goes only to a "const" parameter, which never writes
                   through the view. */
                TypeKind elem = type_elem(arg->type);
                int id = ++held.next_id;
                fprintf(out, "__extension__ ({ %s *wind_v%d = (%s *)&", c_type(arg->type), id,
                        c_type(arg->type));
                emit_whole(arg, out);
                fputs("; (", out);
                emit_slice_type(elem, out);
                if (runtime_length(arg->type)) {
                    fprintf(out, "){ wind_v%d->data, wind_v%d->len }; })", id, id);
                } else {
                    fprintf(out, "){ wind_v%d->v, %zu }; })", id, type_length(arg->type));
                }
            }
        }
        fputs(cast_result ? "))" : ")", out);
        break;
    }
    case EXPR_CAST:
        if (expr->type == TYPE_STRING && is_whole(expr->as.cast.operand)) {
            /* A view of the characters an array of char holds now. */
            const Expr *operand = expr->as.cast.operand;
            int id = ++held.next_id;
            fprintf(out, "__extension__ ({ %s *wind_c%d = &", c_type(operand->type), id);
            emit_whole(operand, out);
            fprintf(out, "; (%s){ (const char *)wind_c%d->data, wind_c%d->len }; })",
                    c_type(TYPE_STRING), id, id);
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
        if (bounds_checked && type_is_float(expr->as.cast.operand->type) &&
            (type_is_integer(expr->type) || expr->type == TYPE_CHAR)) {
            /* A float that does not fit stops the program; C leaves it
               undefined. A value fits if it is truncated to one. */
            used.helpers[HELPER_FLOAT_FAIL] = true;
            int id = ++held.next_id;
            int bits = (int)type_size(expr->type) * 8;
            bool is_signed = type_is_signed(expr->type);
            fprintf(out, "__extension__ ({ long double wind_f%d = (long double)(", id);
            emit_expr(expr->as.cast.operand, out);
            if (is_signed) {
                fprintf(out, "); if (WIND_UNLIKELY(!(wind_f%d > -0x1p%dL - 1 && wind_f%d < 0x1p%dL)))",
                        id, bits - 1, id, bits - 1);
            } else {
                fprintf(out, "); if (WIND_UNLIKELY(!(wind_f%d > -1.0L && wind_f%d < 0x1p%dL)))", id,
                        id, bits);
            }
            fprintf(out, " wind_float_fail(%d, \"%s\"); (%s)wind_f%d; })", expr->loc.line,
                    type_name(expr->type), c_type(expr->type), id);
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
    case EXPR_KEY:
        break; /* only sort takes one, and reads it itself */
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
    if (expr->decayed) {
        /* A whole array standing for a pointer to its first element. */
        Expr whole = *expr;
        whole.decayed = false;
        whole.type = expr->decayed_from;
        fputs("(", out);
        emit_expr_kind(&whole, out);
        fprintf(out, ")%s", elements_of(whole.type));
        return;
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
    TypeInfo *container = type_info(type);
    if (container && container->container) {
        container->writer_used = true;
        container->needed = true;
        *cast = "&";
        return container->writer_name;
    }
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
    if (is_whole(arg)) {
        /* A whole array prints as "[a, b, c]", and a map as "{key: value,
           ...}" in the order its keys were first put. */
        const char *cast;
        const char *writer = value_writer(arg->type, false, &cast);
        fprintf(out, "%s(&", writer);
        emit_whole(arg, out);
        fputs(");\n", out);
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
        Expr base = {.kind = EXPR_IDENT, .type = symbol->type};
        base.as.ident.symbol = symbol;
        emit_element(&base, index, out);
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
    return place->kind == EXPR_INDEX && is_map(place->as.index.base) ? place : NULL;
}

/* Writes into a map: the key, then the value, are worked out first, and
   then the entry is found or put there, and assigned. */
static void emit_map_assign(const Stmt *stmt, const Expr *entry, int depth, FILE *out) {
    TypeKind map = entry->as.index.base->type;
    TypeKind value_type = type_elem(map);
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
    fprintf(out, "; %s *wind_p%d = (%s *)wind_map_put(&", c_type(value_type), id,
            c_type(value_type));
    emit_whole(entry->as.index.base, out);
    fprintf(out, ", &wind_k%d, sizeof(%s), sizeof(%s), %d, %d); ", id, c_type(type_key(map)),
            c_type(value_type), type_key(map) == TYPE_STRING, stmt->loc.line);
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
        case STMT_SWITCH:
            for (size_t j = 0; j < stmt->as.switch_.case_count; j++) {
                if (continues(&stmt->as.switch_.cases[j].body, loop)) return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

static void emit_stmt_kind(const Stmt *stmt, int depth, FILE *out);
static void emit_array_decl(const Stmt *stmt, int depth, FILE *out);

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

/* A case's value as a C constant, whatever constant it was written as. */
static void emit_case_number(long long number, FILE *out) {
    if (number == LLONG_MIN) {
        fputs("(-9223372036854775807LL - 1)", out);
    } else {
        fprintf(out, "%lldLL", number);
    }
}

/* A C switch. On a string, the case that matches is found by comparing the
   string with each case's values in turn, and its position switched on. */
static void emit_switch(const Stmt *stmt, int depth, FILE *out) {
    const Expr *subject = stmt->as.switch_.subject;
    bool is_string = subject->type == TYPE_STRING;
    int inner = depth;
    if (is_string) {
        used.helpers[HELPER_STRING_EQ] = true;
        int id = ++held.next_id;
        indent(depth, out);
        fputs("{\n", out);
        inner = depth + 1;
        indent(inner, out);
        fprintf(out, "wind_string wind_s%d = ", id);
        emit_expr(subject, out);
        fputs(";\n", out);
        indent(inner, out);
        fprintf(out, "int wind_k%d =", id);
        for (size_t i = 0; i < stmt->as.switch_.case_count; i++) {
            const SwitchCase *each = &stmt->as.switch_.cases[i];
            for (size_t j = 0; j < each->value_count; j++) {
                fprintf(out, " wind_string_eq(wind_s%d, ", id);
                emit_expr(each->values[j], out);
                fprintf(out, ") ? %zu :", i);
            }
        }
        fputs(" -1;\n", out);
        indent(inner, out);
        fprintf(out, "switch (wind_k%d) {\n", id);
    } else {
        indent(depth, out);
        fputs("switch (", out);
        emit_expr(subject, out);
        fputs(") {\n", out);
    }
    push_loop(stmt);
    for (size_t i = 0; i < stmt->as.switch_.case_count; i++) {
        const SwitchCase *each = &stmt->as.switch_.cases[i];
        indent(inner, out);
        if (is_string && each->value_count) {
            fprintf(out, "case %zu: ", i);
        }
        for (size_t j = 0; !is_string && j < each->value_count; j++) {
            fputs("case ", out);
            emit_case_number(each->numbers[j], out);
            fputs(": ", out);
        }
        if (each->is_default) fputs("default: ", out);
        fputs("{\n", out);
        emit_block(&each->body, inner + 1, out);
        indent(inner, out);
        fputs(each->falls ? "}\n" : "} break;\n", out);
    }
    loop_marks.count--;
    indent(inner, out);
    fputs("}\n", out);
    if (is_string) {
        indent(depth, out);
        fputs("}\n", out);
    }
}

/* The bitfield a field expression names, or NULL. */
static const Field *bitfield_of(const Expr *expr) {
    if (expr->kind != EXPR_FIELD || expr->as.field.index < 0) return NULL;
    TypeKind base = expr->as.field.base->type;
    if (expr->as.field.through_pointer) base = type_target(base);
    const TypeInfo *info = type_info(base);
    if (!info || (size_t)expr->as.field.index >= info->field_count) return NULL;
    const Field *field = &info->fields[expr->as.field.index];
    return field->bits ? field : NULL;
}

/* Assigns a bitfield, which has no address: the struct it is part of is
   found once, through a pointer, and the field reached from that. */
static void emit_bitfield_assign(const Stmt *stmt, int depth, FILE *out) {
    const Expr *target = stmt->as.assign.target;
    const Expr *value = stmt->as.assign.value;
    const Expr *base = target->as.field.base;
    TypeKind type = target->type;
    Op op = stmt->as.assign.op;
    int id = ++held.next_id;
    TypeKind holder = target->as.field.through_pointer ? type_target(base->type) : base->type;
    indent(depth, out);
    fprintf(out, "{ %s *wind_b%d = ", c_type(holder), id);
    if (target->as.field.through_pointer) {
        emit_checked_pointer(base, out);
    } else {
        fputs("&", out);
        emit_whole(base, out);
    }
    char field[160];
    int length = snprintf(field, sizeof field, "wind_b%d->f_", id);
    snprintf(field + length, sizeof field - (size_t)length, "%.*s",
             (int)target->as.field.name.len, target->as.field.name.data);
    fprintf(out, "; %s = ", field);
    if (!stmt->as.assign.compound) {
        emit_expr(value, out);
    } else if (checked_division(op, type, value)) {
        emit_division(op, type, NULL, field, value, stmt->loc.line, out);
    } else if (op_is_shift(op)) {
        emit_shift_start(op, type, out);
        fputs(field, out);
        emit_shift_end(value, out);
    } else {
        fprintf(out, "((%s)(%s %s ", c_type(type), field, op_text(op));
        emit_expr(value, out);
        fputs("))", out);
    }
    fputs("; }\n", out);
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
    case STMT_SETTING:
    case STMT_FALLTHROUGH: /* a C case runs on into the next by itself */
        break;
    case STMT_SWITCH:
        emit_switch(stmt, depth, out);
        break;
    case STMT_GROUP:
        /* Left only in a for loop's clauses. */
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            emit_stmt(stmt->as.block.stmts[i], depth, out);
        }
        break;
    case STMT_VAR:
        if (stmt->is_static) break; /* see emit_variables */
        indent(depth, out);
        if (stmt->as.decl.is_array) {
            emit_array_decl(stmt, depth, out);
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
        if (bitfield_of(target)) {
            emit_bitfield_assign(stmt, depth, out);
            break;
        }
        if (stmt->as.assign.compound && checked_division(stmt->as.assign.op, target->type,
                                                         value)) {
            /* "x /= v": the place is found first, then divided as "/" is. */
            int id = ++held.next_id;
            indent(depth, out);
            fprintf(out, "{ %s *wind_p%d = &", c_type(target->type), id);
            emit_expr(target, out);
            fprintf(out, "; *wind_p%d = ", id);
            char left_text[32];
            snprintf(left_text, sizeof left_text, "(*wind_p%d)", id);
            emit_division(stmt->as.assign.op, target->type, NULL, left_text, value,
                          stmt->loc.line, out);
            fputs("; }\n", out);
            break;
        }
        if (stmt->as.assign.compound && target->type == TYPE_STRING) {
            /* "s += v": the place is found once, and v joined to it. */
            int id = ++held.next_id;
            size_t mark = held.count;
            indent(depth, out);
            fprintf(out, "{ %s *wind_p%d = &", c_type(TYPE_STRING), id);
            emit_expr(target, out);
            fprintf(out, "; *wind_p%d = ", id);
            hold(target, id, true);
            const Expr *parts[2] = {target, value};
            used.helpers[HELPER_SCRATCH] = true;
            int join = ++held.next_id;
            fprintf(out, "__extension__ ({ wind_list_char wind_j%d = {0}; ", join);
            char into[32];
            snprintf(into, sizeof into, "&wind_j%d", join);
            emit_written_into((Expr *const *)parts, 2, into, stmt->loc.line, false, out);
            fprintf(out, " wind_keep(&wind_j%d, %d); }); }\n", join, stmt->loc.line);
            held.count = mark;
            break;
        }
        if (type_container(target->type) == CONTAINER_LIST) {
            /* The new elements are made before the old ones are freed,
               since they may be a copy of them. */
            int id = ++held.next_id;
            used.headers |= HEADER_STDLIB;
            indent(depth, out);
            fprintf(out, "{ %s wind_copy%d = ", c_type(target->type), id);
            emit_expr(value, out);
            fprintf(out, "; %s *wind_p%d = &", c_type(target->type), id);
            emit_whole(target, out);
            fprintf(out, "; free(wind_p%d->data); *wind_p%d = wind_copy%d; }\n", id, id, id);
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
        bool map = is_map(over);
        bool array = !map && is_whole(over);
        size_t mark = held.count;
        indent(depth, out);
        fputs("{\n", out);
        if (!map && !array) {
            indent(depth + 1, out);
            fprintf(out, "%s wind_s%d = ", c_type(TYPE_STRING), id);
            emit_expr(over, out);
            fputs(";\n", out);
        } else if (!is_plain(over)) {
            /* Reached once, through a pointer. */
            indent(depth + 1, out);
            fprintf(out, "%s *wind_p%d = &", c_type(over->type), id);
            emit_whole(over, out);
            fputs(";\n", out);
            hold(over, id, true);
        }
        indent(depth + 1, out);
        fprintf(out, "for (size_t wind_i%d = 0; wind_i%d < ", id, id);
        if (map) {
            used.helpers[HELPER_MAP] = true;
            emit_whole(over, out);
            fputs(".used", out);
        } else if (array && runtime_length(over->type)) {
            emit_whole(over, out);
            fputs(".len", out);
        } else if (array) {
            fprintf(out, "%zu", type_length(over->type));
        } else {
            fprintf(out, "wind_s%d.len", id);
        }
        fprintf(out, "; wind_i%d++) {\n", id);
        if (map) {
            indent(depth + 2, out);
            fputs("if (!", out);
            emit_whole(over, out);
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
                fprintf(out, "((%s *)", c_type(i == 0 ? type_key(over->type)
                                                     : type_elem(over->type)));
                emit_whole(over, out);
                fprintf(out, ".%s)[wind_i%d];\n", i == 0 ? "keys" : "values", id);
            } else if (array) {
                emit_whole(over, out);
                fprintf(out, "%s[wind_i%d];\n", elements_of(over->type), id);
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
        held.count = mark;
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
    case STMT_SCRATCH:
        used.helpers[HELPER_SCRATCH] = true;
        indent(depth, out);
        if (stmt->as.scratch.free) {
            fprintf(out, "wind_scratch_free(wind_mark%d);\n", stmt->as.scratch.id);
        } else {
            fprintf(out, "size_t wind_mark%d = wind_scratch_mark();\n", stmt->as.scratch.id);
        }
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

/* "T name[n] = { ... }", "T name[..]", "V name[K] = { ... }" and the
   like, after the indent: what each starts out holding. */
static void emit_array_decl(const Stmt *stmt, int depth, FILE *out) {
    const Symbol *symbol = stmt->as.decl.symbol;
    TypeKind type = symbol->type;
    size_t count = stmt->as.decl.elem_count;
    if (type_container(type) == CONTAINER_MAP) {
        /* Empty, and then the entries given, in order. */
        TypeKind value = type_elem(type);
        used.helpers[count ? HELPER_MAP_PUT : HELPER_MAP] = true;
        emit_symbol(symbol, out);
        fputs(" = (wind_map){0};\n", out);
        for (size_t i = 0; i + 1 < count; i += 2) {
            int id = ++held.next_id;
            indent(depth, out);
            fputs("{ ", out);
            emit_key_temp(type, stmt->as.decl.elems[i], id, out);
            fprintf(out, "%s wind_t%d = ", c_type(value), id);
            emit_expr(stmt->as.decl.elems[i + 1], out);
            fprintf(out, "; *(%s *)wind_map_put(&", c_type(value));
            emit_symbol(symbol, out);
            fprintf(out, ", &wind_k%d, sizeof(%s), sizeof(%s), %d, %d) = wind_t%d; }\n", id,
                    c_type(type_key(type)), c_type(value), type_key(type) == TYPE_STRING,
                    stmt->loc.line, id);
        }
        return;
    }
    if (type_container(type) == CONTAINER_LIST) {
        /* Empty, or a copy, and then the elements given, in order. */
        TypeKind elem = type_elem(type);
        emit_symbol(symbol, out);
        fputs(" = ", out);
        if (stmt->as.decl.init) {
            emit_expr(stmt->as.decl.init, out);
        } else {
            fprintf(out, "(%s){0}", c_type(type));
        }
        fputs(";\n", out);
        if (count) {
            used.helpers[HELPER_GROW] = true;
            indent(depth, out);
            emit_symbol(symbol, out);
            fputs(".data = wind_grow(", out);
            emit_symbol(symbol, out);
            fputs(".data, &", out);
            emit_symbol(symbol, out);
            fputs(".cap, ", out);
            emit_symbol(symbol, out);
            fprintf(out, ".len + %zu, sizeof(%s), %d);\n", count, c_type(elem), stmt->loc.line);
            for (size_t i = 0; i < count; i++) {
                indent(depth, out);
                emit_symbol(symbol, out);
                fputs(".data[", out);
                emit_symbol(symbol, out);
                fputs(".len++] = ", out);
                emit_expr(stmt->as.decl.elems[i], out);
                fputs(";\n", out);
            }
        }
        return;
    }
    /* A fixed array: elements the initializer leaves out are zero, as in C. */
    if (count == 0 && !stmt->as.decl.init && symbol->global &&
        !top_level_repeats(symbol->unit)) {
        /* Its storage starts out zero and this runs once, so there is
           nothing to clear. */
        fputs(";\n", out);
        return;
    }
    emit_symbol(symbol, out);
    fputs(" = ", out);
    if (stmt->as.decl.init) {
        emit_expr(stmt->as.decl.init, out);
    } else {
        fprintf(out, "(%s){ { ", c_type(type));
        for (size_t i = 0; i < count; i++) {
            if (i) fputs(", ", out);
            emit_expr(stmt->as.decl.elems[i], out);
        }
        fputs(count ? " } }" : "0 } }", out);
    }
    fputs(";\n", out);
}

static void emit_static_value(const Expr *expr, FILE *out);

/* Declares variables, each zero; a static one in a function starts out as
   its declaration says, once, as C's does. */
static void emit_variables(const SymbolList *variables, const char *prefix, FILE *out) {
    for (size_t i = 0; i < variables->count; i++) {
        const Symbol *variable = variables->items[i];
        fprintf(out, "%s%s ", prefix, c_type(variable->type));
        emit_symbol(variable, out);
        const Stmt *decl = variable->is_static ? variable->decl : NULL;
        if (decl && decl->as.decl.is_array && decl->as.decl.elem_count) {
            fputs(" = { { ", out);
            for (size_t j = 0; j < decl->as.decl.elem_count; j++) {
                if (j) fputs(", ", out);
                emit_static_value(decl->as.decl.elems[j], out);
            }
            fputs(" } };\n", out);
        } else if (decl && !decl->as.decl.is_array && decl->as.decl.init) {
            fputs(" = ", out);
            emit_static_value(decl->as.decl.init, out);
            fputs(";\n", out);
        } else {
            fputs(" = {0};\n", out);
        }
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
        ContainerKind kind = type_container(param->type);
        fprintf(out, "%s %s", c_type(param->type),
                kind == CONTAINER_LIST || kind == CONTAINER_MAP ? "*" : "");
        emit_symbol_name(param->symbol, out);
    }
    fputs(")", out);
}

/* Writes the headers, type definitions and helpers recorded in `used`. */
static void emit_runtime(FILE *out) {
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
    } else if (expr->kind == EXPR_INIT && type_is_container(expr->type)) {
        fputs("{ { ", out);
        for (size_t i = 0; i < expr->as.init.count; i++) {
            if (i) fputs(", ", out);
            emit_static_value(expr->as.init.items[i].value, out);
        }
        fputs(expr->as.init.count ? " } }" : "0 } }", out);
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
        if (type_is_container(symbol->type)) {
            fputs(" = { { ", out);
            for (size_t j = 0; j < decl->as.decl.elem_count; j++) {
                if (j) fputs(", ", out);
                emit_static_value(decl->as.decl.elems[j], out);
            }
            fputs(" } };\n", out);
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
    if (func->as.func.param_count == 0 && !func->as.func.variadic) fputs("void", out);
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        fprintf(out, "%s%s", i ? ", " : "", c_type(func->as.func.params[i].type));
    }
    fputs(func->as.func.variadic ? ", ...);\n" : ");\n", out);
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
    emitting_unit = unit;
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
        emitting_unit = unit->modules[i];
        emit_block(&unit->modules[i]->program.top, 1, out);
    }
    emitting_unit = unit;
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

static void define_type(TypeKind type, FILE *out);

/* Makes a type's C name usable from here on. A struct's and an enum's
   always are; see emit_declared_types. */
static void name_type(TypeKind type, FILE *out) {
    if (type_is_struct(type) || type_is_enum(type)) return;
    define_type(type, out);
}

/* Defines a type in C, once, after what it holds by value; what it only
   points to need only be named. An array or a view of a built-in type is
   left to the runtime, whose helpers use some of them. */
static void define_type(TypeKind type, FILE *out) {
    TypeInfo *info = type_info(type);
    if (!info) {
        if (type < TYPE_COUNT) used.types[type] = true;
        return;
    }
    if (info->defined) return;
    info->defined = true;
    if (info->is_enum) return;
    if (info->is_pointer) {
        name_type(info->target, out);
        fprintf(out, "typedef %s%s *%s;\n", info->read_only ? "const " : "",
                type_c_name(info->target), info->c_name);
        return;
    }
    if (info->is_function) {
        /* As the functions it points to are declared: a growable array or a
           map is taken as a pointer to the caller's own. */
        name_type(info->target, out);
        for (size_t i = 0; i < info->param_count; i++) name_type(info->params[i], out);
        fprintf(out, "typedef %s (*%s)(", type_c_name(info->target), info->c_name);
        for (size_t i = 0; i < info->param_count; i++) {
            ContainerKind kind = type_container(info->params[i]);
            fprintf(out, "%s%s%s", i ? ", " : "", type_c_name(info->params[i]),
                    kind == CONTAINER_LIST || kind == CONTAINER_MAP ? " *" : "");
        }
        fputs(info->param_count ? ");\n" : "void);\n", out);
        return;
    }
    switch (info->container) {
    case CONTAINER_MAP:
        used.helpers[HELPER_MAP] = true;
        name_type(info->key, out);
        name_type(info->target, out);
        return;
    case CONTAINER_LIST:
    case CONTAINER_SLICE:
        used.headers |= HEADER_STDDEF;
        if (info->target < TYPE_COUNT) {
            used.types[info->target] = true;
            if (info->container == CONTAINER_LIST) {
                used.lists[info->target] = true;
            } else {
                used.slices[info->target] = true;
            }
            return;
        }
        name_type(info->target, out);
        fprintf(out, "typedef struct {\n    %s *data;\n    size_t len;\n%s} %s;\n",
                type_c_name(info->target),
                info->container == CONTAINER_LIST ? "    size_t cap;\n" : "", info->c_name);
        return;
    case CONTAINER_ARRAY:
        /* In a struct, so that it is copied, passed and returned as one. */
        define_type(info->target, out);
        fprintf(out, "typedef struct {\n    %s v[%zu];\n} %s;\n", type_c_name(info->target),
                info->length, info->c_name);
        return;
    default:
        break;
    }
    for (size_t j = 0; j < info->field_count; j++) define_type(info->fields[j].type, out);
    fprintf(out, "%s %s {\n", info->is_union ? "union" : "struct", info->c_name);
    for (size_t j = 0; j < info->field_count; j++) {
        fprintf(out, "    %s ", type_c_name(info->fields[j].type));
        emit_field_name(info->fields[j].name, out);
        if (info->fields[j].bits) fprintf(out, " : %d", info->fields[j].bits);
        fputs(";\n", out);
    }
    fputs("};\n", out);
}

/* The writer of a whole array or map: "[a, b, c]", or "{key: value, ...}"
   in the order its keys were first put. */
static void emit_container_writer(const TypeInfo *info, FILE *out) {
    const char *cast;
    const char *writer = value_writer(info->target, true, &cast);
    fprintf(out, "static void %s(const %s *value) {\n", info->writer_name, info->c_name);
    if (info->container == CONTAINER_MAP) {
        const char *key_cast;
        const char *key_writer = value_writer(info->key, true, &key_cast);
        fprintf(out,
                "    int first = 1;\n"
                "    wind_put(\"{\", 1);\n"
                "    for (size_t i = 0; i < value->used; i++) {\n"
                "        if (!value->live[i]) continue;\n"
                "        if (!first) wind_put(\", \", 2);\n"
                "        first = 0;\n"
                "        %s(%s((%s *)value->keys)[i]);\n"
                "        wind_put(\": \", 2);\n"
                "        %s(%s((%s *)value->values)[i]);\n"
                "    }\n"
                "    wind_put(\"}\", 1);\n"
                "}\n\n",
                key_writer, key_cast, type_c_name(info->key), writer, cast,
                type_c_name(info->target));
        return;
    }
    bool fixed = info->container == CONTAINER_ARRAY;
    char length[32];
    snprintf(length, sizeof length, "%zu", info->length);
    fprintf(out,
            "    wind_put(\"[\", 1);\n"
            "    for (size_t i = 0; i < %s; i++) {\n"
            "        if (i) wind_put(\", \", 2);\n"
            "        %s(%svalue->%s[i]);\n"
            "    }\n"
            "    wind_put(\"]\", 1);\n"
            "}\n\n",
            fixed ? length : "value->len", writer, cast, fixed ? "v" : "data");
}

/* Emits what the program's types need: the definitions of its structs and
   pointer types, and of the arrays and maps it names, each after what it
   holds; then the printing functions that the program turned out to use.
   Must run after the program is emitted, which settles what is used, and
   before the runtime is, which this adds to. */
static void emit_declared_types(FILE *out) {
    /* Every struct is named before any is defined, so that a pointer can
       point to a struct defined further down, or to its own. */
    for (size_t i = 0; i < types_count(); i++) {
        TypeKind type = (TypeKind)(TYPE_COUNT + i);
        const TypeInfo *info = type_info(type);
        if (info->is_enum) {
            used.headers |= HEADER_STDINT;
            fprintf(out, "typedef int32_t %s;\n", info->c_name);
        } else if (type_is_struct(type)) {
            fprintf(out, "typedef %s %s %s;\n", info->is_union ? "union" : "struct",
                    info->c_name, info->c_name);
        }
    }
    /* Printing a value may print values of types that nothing else
       prints, or names, so the sets are grown until they stop changing. */
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < types_count(); i++) {
            TypeKind type = (TypeKind)(TYPE_COUNT + i);
            TypeInfo *info = type_info(type);
            if (!info->writer_used || info->writer_emitted || info->is_pointer) continue;
            info->writer_emitted = true;
            changed = true;
            used.helpers[HELPER_PUT] = true;
            const char *cast;
            if (info->is_enum) used.helpers[HELPER_WRITE_INT] = true;
            if (info->container) {
                value_writer(info->target, true, &cast);
                if (info->container == CONTAINER_MAP) value_writer(info->key, true, &cast);
            }
            for (size_t j = 0; j < info->field_count; j++) {
                value_writer(info->fields[j].type, true, &cast);
            }
        }
    }
    for (size_t i = 0; i < types_count(); i++) {
        TypeKind type = (TypeKind)(TYPE_COUNT + i);
        TypeInfo *info = type_info(type);
        if (!info->container || info->needed || info->writer_used) define_type(type, out);
    }
    if (types_count()) fputs("\n", out);

    for (size_t i = 0; i < types_count(); i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->writer_used || info->is_pointer) continue;
        if (info->container) {
            fprintf(out, "static void %s(const %s *value);\n", info->writer_name, info->c_name);
        } else {
            fprintf(out, "static void %s(%s value);\n", info->writer_name, info->c_name);
        }
    }
    for (size_t i = 0; i < types_count(); i++) {
        const TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        if (!info->writer_used || info->is_pointer) continue;
        if (info->container) {
            emit_container_writer(info, out);
            continue;
        }
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

/* Notes the types that an extern in a carried header takes and gives,
   which the header's C refers to whether or not the program calls it. */
static void use_carried_types(const Stmt *func) {
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        c_type(func->as.func.params[i].type);
    }
    if (!type_info(func->as.func.ret)) used.types[func->as.func.ret] = true;
}

static void use_carried_in(const Unit *unit) {
    const Block *top = &unit->program.top;
    for (size_t i = 0; i < top->count; i++) {
        const Stmt *stmt = top->stmts[i];
        if (stmt->kind != STMT_FUNC || !stmt->carried.len) continue;
        for (size_t j = 0; j < carried.count; j++) {
            if (str_eq(carried.items[j], stmt->carried)) use_carried_types(stmt);
        }
    }
}

/* The leading "#define" lines of a carried header, which must come before
   any system header is included to have their effect, and then the rest. */
static void split_carried(Str text, Str *defines, Str *rest) {
    size_t at = 0, end = 0;
    while (at < text.len) {
        size_t line_end = at;
        while (line_end < text.len && text.data[line_end] != '\n') line_end++;
        Str line = {text.data + at, line_end - at};
        size_t i = 0;
        while (i < line.len && (line.data[i] == ' ' || line.data[i] == '\t')) i++;
        bool blank = i == line.len;
        bool comment = i + 1 < line.len && line.data[i] == '/' &&
                       (line.data[i + 1] == '/' || line.data[i + 1] == '*');
        bool inside = i < line.len && (line.data[i] == '*' );
        bool define = line.len - i >= 7 && memcmp(line.data + i, "#define", 7) == 0;
        if (!blank && !comment && !inside && !define) break;
        if (define) end = line_end + 1 < text.len ? line_end + 1 : text.len;
        at = line_end + 1;
    }
    *defines = (Str){text.data, end};
    *rest = (Str){text.data + end, text.len - end};
}

static void emit_types_of(const Unit *unit, FILE *out) {
    (void)unit;
    emit_declared_types(out);
    emit_orders(out);
}

void emit_c(const Unit *unit, FILE *out, EmitOptions options) {
    target = options.target ? options.target : &target_host;
    bounds_checked = !options.no_bounds_check;
    free(used.externals);
    memset(&used, 0, sizeof used);
    orders.count = 0;
    carried.count = 0;
    for (size_t i = 0; i < links.count; i++) free(links.items[i]);
    links.count = 0;
    types_use((TypeTable *)&unit->types);
    for (size_t i = 0; i < types_count(); i++) {
        TypeInfo *info = type_info((TypeKind)(TYPE_COUNT + i));
        info->writer_used = info->writer_emitted = false;
        info->needed = info->defined = false;
    }

    /* Written in the reverse of the order they appear in, since each part
       finds out what the parts before it must provide. */
    size_t program_len, types_len;
    char *program = emit_to_memory(emit_program, unit, &program_len);
    for (size_t i = 0; i < unit->module_count; i++) use_carried_in(unit->modules[i]);
    use_carried_in(unit);
    char *types = emit_to_memory(emit_types_of, unit, &types_len);
    for (size_t i = 0; i < carried.count; i++) {
        Str defines, rest;
        split_carried(carried.items[i], &defines, &rest);
        fwrite(defines.data, 1, defines.len, out);
    }
    if (target->prologue) fputs(target->prologue, out);
    emit_runtime(out);
    fwrite(types, 1, types_len, out);
    /* A carried header reads Wind's types from here; on its own, as an
       editor reads it, from libs/wind.h. */
    if (carried.count) fputs("#define WIND_PROGRAM 1\n", out);
    for (size_t i = 0; i < carried.count; i++) {
        Str defines, rest;
        split_carried(carried.items[i], &defines, &rest);
        fwrite(rest.data, 1, rest.len, out);
        fputs("\n", out);
    }
    fwrite(program, 1, program_len, out);
    free(types);
    free(program);
}
