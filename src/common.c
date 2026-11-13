#define _DEFAULT_SOURCE

#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool str_eq(Str a, Str b) {
    return a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

bool str_eq_cstr(Str a, const char *b) {
    return a.len == strlen(b) && memcmp(a.data, b, a.len) == 0;
}

void *xmalloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) fatal("out of memory");
    return ptr;
}

void *xrealloc(void *ptr, size_t size) {
    ptr = realloc(ptr, size ? size : 1);
    if (!ptr) fatal("out of memory");
    return ptr;
}

void fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("wind: error: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

/* ---- Arena ---- */

#define ARENA_ALIGN _Alignof(max_align_t)
#define ARENA_ROUND(n) (((n) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1))
#define ARENA_CHUNK_SIZE ((size_t)64 * 1024)

struct ArenaChunk {
    ArenaChunk *next;
    size_t used;
    size_t cap;
};

static char *chunk_data(ArenaChunk *chunk) {
    return (char *)chunk + ARENA_ROUND(sizeof(ArenaChunk));
}

void *arena_alloc(Arena *arena, size_t size) {
    size = ARENA_ROUND(size ? size : 1);
    ArenaChunk *chunk = arena->chunks;
    if (!chunk || chunk->cap - chunk->used < size) {
        size_t cap = size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE;
        chunk = xmalloc(ARENA_ROUND(sizeof(ArenaChunk)) + cap);
        chunk->next = arena->chunks;
        chunk->used = 0;
        chunk->cap = cap;
        arena->chunks = chunk;
    }
    void *ptr = chunk_data(chunk) + chunk->used;
    chunk->used += size;
    return ptr;
}

void *arena_grow(Arena *arena, void *old, size_t old_size, size_t new_size) {
    void *ptr = arena_alloc(arena, new_size);
    if (old_size) memcpy(ptr, old, old_size);
    return ptr;
}

void arena_release(Arena *arena) {
    ArenaChunk *chunk = arena->chunks;
    while (chunk) {
        ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->chunks = NULL;
}

/* ---- Diagnostics ---- */

const char *severity_name(Severity severity) {
    return severity == SEV_ERROR ? "error" : "warning";
}

void diag_add(Diags *diags, Severity severity, SrcLoc loc, const char *fmt, ...) {
    va_list args, measure;
    va_start(args, fmt);
    va_copy(measure, args);
    int len = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    char *message = arena_alloc(diags->arena, (size_t)len + 1);
    vsnprintf(message, (size_t)len + 1, fmt, args);
    va_end(args);

    Diag diag = {.severity = severity, .loc = loc, .message = message};
    ARENA_PUSH(diags->arena, diags->items, diags->count, diags->cap, diag);
    if (severity == SEV_ERROR) diags->error_count++;
}

void diag_mark_unnecessary(Diags *diags, SrcLoc end) {
    Diag *diag = &diags->items[diags->count - 1];
    diag->unnecessary = true;
    diag->end = end;
}

void diags_sort(Diags *diags) {
    for (size_t i = 1; i < diags->count; i++) {
        Diag d = diags->items[i];
        size_t j = i;
        while (j > 0 && diags->items[j - 1].loc.offset > d.loc.offset) {
            diags->items[j] = diags->items[j - 1];
            j--;
        }
        diags->items[j] = d;
    }
}

bool file_read(const char *path, Str *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    if (ferror(f)) {
        /* Reading a directory fails here, not at fopen. */
        int error = errno;
        fclose(f);
        free(buf);
        errno = error;
        return false;
    }
    fclose(f);
    *out = (Str){buf, len};
    return true;
}

char *path_beside(const char *from, Str written) {
    const char *slash = from ? strrchr(from, '/') : NULL;
    size_t dir_len = slash ? (size_t)(slash - from) + 1 : 0;
    if (written.len > 0 && written.data[0] == '/') dir_len = 0;
    char *path = xmalloc(dir_len + written.len + 1);
    if (dir_len) memcpy(path, from, dir_len);
    memcpy(path + dir_len, written.data, written.len);
    path[dir_len + written.len] = '\0';
    return path;
}

char *path_canonical(const char *path) {
    return realpath(path, NULL);
}
