#ifndef WIND_COMMON_H
#define WIND_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#define WIND_VERSION "0.1.0"

/* A length-delimited byte slice. Never assumed to be NUL-terminated. */
typedef struct {
    const char *data;
    size_t len;
} Str;

bool str_eq(Str a, Str b);
bool str_eq_cstr(Str a, const char *b);

/* A region allocator: everything allocated from an arena is released at once.
   A zero-initialized Arena is empty and ready to use. */
typedef struct ArenaChunk ArenaChunk;
typedef struct {
    ArenaChunk *chunks;
} Arena;

void *arena_alloc(Arena *arena, size_t size);
/* Returns a block of new_size bytes holding the first old_size bytes of old. */
void *arena_grow(Arena *arena, void *old, size_t old_size, size_t new_size);
void arena_release(Arena *arena);

/* A region of one source line. line and col are 1-based; col, offset and len
   count bytes. */
typedef struct {
    size_t offset;
    size_t len;
    int line;
    int col;
} SrcLoc;

typedef enum {
    SEV_ERROR,
    SEV_WARNING,
} Severity;

typedef struct {
    Severity severity;
    SrcLoc loc;
    const char *message;
} Diag;

typedef struct {
    Arena *arena;
    Diag *items;
    size_t count;
    size_t cap;
    size_t error_count;
} Diags;

void diag_add(Diags *diags, Severity severity, SrcLoc loc, const char *fmt, ...);
/* Orders diagnostics by source position, keeping report order among equals. */
void diags_sort(Diags *diags);
const char *severity_name(Severity severity);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);

/* For failures of the tool itself (I/O, memory), not of the program being
   compiled; those go through diag_add. */
_Noreturn void fatal(const char *fmt, ...);

#endif
