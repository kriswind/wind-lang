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

/* Reads a whole file into memory from malloc. Returns false, with errno
   set, if it cannot be read. */
bool file_read(const char *path, Str *out);

/* The path that "written" names when it appears in the file at "from": taken
   from that file's directory unless it is absolute. from may be NULL, for the
   current directory. The result comes from malloc. */
char *path_beside(const char *from, Str written);

/* The directory of the libraries that an import reaches with "::", found
   from program, the path the compiler was started as. It is $WIND_LIBS if
   that is set; otherwise "libs" beside the directory the compiler is in,
   which is where it is in a build of the source, or "share/wind/libs" there,
   which is where "make install" puts it. */
void libs_locate(const char *program);
/* The directory libs_locate found, or NULL if there is none. */
const char *libs_dir(void);

/* The file an import names, from malloc: "::path" in the libraries
   directory, any other path beside the file at "from" as path_beside finds
   it, and ".wind" added unless the path ends with it. Returns NULL if the
   path starts with "::" and there is no libraries directory. */
char *path_of_import(const char *from, Str written);

/* The absolute path of an existing file with every link resolved, from
   malloc, so that two spellings of one file compare equal. Returns NULL,
   with errno set, if there is no such file. */
char *path_canonical(const char *path);

/* Runs the program at path, searched for in PATH if it has no slash, with
   the given NULL-terminated argument vector, and waits for it. Returns its
   exit code, or 128 + the signal number if a signal ended it. A signal
   that would end wind while it waits (an interrupt, a hangup, a request to
   terminate) is passed on to the program instead, so that wind outlives it
   and can clean up after it. */
int run_program(const char *path, char *const argv[]);
/* Runs a program as run_program does, with its standard output captured.
   Returns what it wrote there, from malloc and NUL-terminated, and sets
   *status to what run_program would return. */
char *program_output(const char *path, char *const argv[], int *status);
/* True if a program of this name can be found in PATH. */
bool program_on_path(const char *name);
/* The directory for temporary files: $TMPDIR, or /tmp. */
const char *temp_dir(void);

/* For failures of the tool itself (I/O, memory), not of the program being
   compiled; those go through diag_add. */
_Noreturn void fatal(const char *fmt, ...);

#endif
