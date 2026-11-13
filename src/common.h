#ifndef WIND_COMMON_H
#define WIND_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#define WIND_VERSION "0.2.0"

/* A length-delimited byte slice. Never assumed to be NUL-terminated. */
typedef struct {
    const char *data;
    size_t len;
} Str;

/* Wide enough for any integer literal; i128 and u128 need all of it. */
__extension__ typedef unsigned __int128 U128;

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

/* Appends item to a growable arena-backed array given as its pointer, count
   and capacity variables. */
#define ARENA_PUSH(arena, items, count, cap, item)                            \
    do {                                                                      \
        if ((count) == (cap)) {                                               \
            size_t new_cap_ = (cap) ? (cap) * 2 : 8;                          \
            (items) = arena_grow((arena), (items), (count) * sizeof *(items), \
                                 new_cap_ * sizeof *(items));                 \
            (cap) = new_cap_;                                                 \
        }                                                                     \
        (items)[(count)++] = (item);                                          \
    } while (0)

/* A region of source text that starts and ends on one line. line and col are
   1-based; col, offset and len count bytes. */
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
    /* Set for code that has no effect, such as code that can never run.
       Editors show it faded instead of underlined. The code may cover
       several lines: it runs from loc to the end of the token at end. */
    bool unnecessary;
    SrcLoc end;
} Diag;

typedef struct {
    Arena *arena;
    Diag *items;
    size_t count;
    size_t cap;
    size_t error_count;
} Diags;

void diag_add(Diags *diags, Severity severity, SrcLoc loc, const char *fmt, ...);
/* Marks the most recently added diagnostic as covering unnecessary code
   that runs up to the end of the token at end. */
void diag_mark_unnecessary(Diags *diags, SrcLoc end);
/* Orders diagnostics by source position, keeping report order among equals. */
void diags_sort(Diags *diags);
const char *severity_name(Severity severity);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);

/* For failures of the tool itself (I/O, memory), not of the program being
   compiled; those go through diag_add. */
_Noreturn void fatal(const char *fmt, ...);

#endif
