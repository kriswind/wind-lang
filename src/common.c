#define _DEFAULT_SOURCE

#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* The program run_program is waiting for, which signals are passed to. */
static volatile pid_t waited_for;

static void pass_on(int signal) {
    if (waited_for > 0) kill(waited_for, signal);
}

int run_program(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) fatal("fork failed: %s", strerror(errno));
    if (pid == 0) {
        execvp(path, argv);
        fprintf(stderr, "wind: error: cannot execute '%s': %s\n", path, strerror(errno));
        _exit(127);
    }
    /* Installed only after the fork, so that the program itself keeps the
       usual response to these signals. */
    static const int passed[] = {SIGINT, SIGHUP, SIGTERM, SIGQUIT};
    struct sigaction pass = {0}, before[sizeof passed / sizeof passed[0]];
    pass.sa_handler = pass_on;
    waited_for = pid;
    for (size_t i = 0; i < sizeof passed / sizeof passed[0]; i++) {
        sigaction(passed[i], &pass, &before[i]);
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    for (size_t i = 0; i < sizeof passed / sizeof passed[0]; i++) {
        sigaction(passed[i], &before[i], NULL);
    }
    waited_for = 0;
    if (waited < 0) fatal("waitpid failed: %s", strerror(errno));
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

char *program_output(const char *path, char *const argv[], int *status) {
    int pipe_ends[2];
    if (pipe(pipe_ends) != 0) fatal("cannot create a pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) fatal("fork failed: %s", strerror(errno));
    if (pid == 0) {
        dup2(pipe_ends[1], STDOUT_FILENO);
        close(pipe_ends[0]);
        close(pipe_ends[1]);
        execvp(path, argv);
        fprintf(stderr, "wind: error: cannot execute '%s': %s\n", path, strerror(errno));
        _exit(127);
    }
    close(pipe_ends[1]);
    size_t len = 0, cap = 4096;
    char *text = xmalloc(cap);
    for (;;) {
        if (len + 1 == cap) text = xrealloc(text, cap *= 2);
        ssize_t got = read(pipe_ends[0], text + len, cap - len - 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        len += (size_t)got;
    }
    text[len] = '\0';
    close(pipe_ends[0]);
    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno != EINTR) fatal("waitpid failed: %s", strerror(errno));
    }
    *status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128 + WTERMSIG(wait_status);
    return text;
}

bool program_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return false;
    while (*path) {
        size_t len = strcspn(path, ":");
        char *candidate = xmalloc(len + strlen(name) + 2);
        /* An empty entry in PATH is the current directory. */
        sprintf(candidate, "%.*s/%s", len ? (int)len : 1, len ? path : ".", name);
        bool found = access(candidate, X_OK) == 0;
        free(candidate);
        if (found) return true;
        path += len;
        if (*path == ':') path++;
    }
    return false;
}

const char *temp_dir(void) {
    const char *dir = getenv("TMPDIR");
    return (dir && *dir) ? dir : "/tmp";
}

static char *libs;

bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

bool is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

void libs_locate(const char *program) {
    free(libs);
    libs = NULL;
    const char *set = getenv("WIND_LIBS");
    if (set && *set) {
        libs = xmalloc(strlen(set) + 1);
        strcpy(libs, set);
        return;
    }
    /* The compiler's own file, through /proc where there is one, so that
       a compiler found through PATH or a link is located as well. */
    char *self = path_canonical("/proc/self/exe");
    if (!self && program && strchr(program, '/')) self = path_canonical(program);
    if (!self) return;
    char *slash = strrchr(self, '/');
    if (slash) *slash = '\0';
    static const char *const beside[] = {"/../libs", "/../share/wind/libs"};
    for (size_t i = 0; i < sizeof beside / sizeof beside[0] && !libs; i++) {
        char *candidate = xmalloc(strlen(self) + strlen(beside[i]) + 1);
        sprintf(candidate, "%s%s", self, beside[i]);
        if (is_directory(candidate)) libs = path_canonical(candidate);
        free(candidate);
    }
    free(self);
}

const char *libs_dir(void) {
    return libs;
}

char *import_base(const char *from, Str written, bool *folder_only) {
    *folder_only = written.len > 0 && written.data[written.len - 1] == '/';
    while (written.len > 1 && written.data[written.len - 1] == '/') written.len--;
    if (written.len >= 2 && written.data[0] == ':' && written.data[1] == ':') {
        if (!libs) return NULL;
        Str rest = {written.data + 2, written.len - 2};
        char *found = xmalloc(strlen(libs) + rest.len + 2);
        sprintf(found, "%s/%.*s", libs, (int)rest.len, rest.data);
        return found;
    }
    return path_beside(from, written);
}

typedef struct {
    char **items;
    size_t count;
    size_t cap;
    size_t folders; /* looked through so far */
    bool too_large;
} PathList;

static void collect_wind_files(const char *folder, PathList *list, int depth) {
    if (list->too_large) return;
    if (++list->folders > WIND_FOLDER_LIMIT || depth > WIND_FOLDER_DEPTH) {
        list->too_large = true;
        return;
    }
    DIR *entries = opendir(folder);
    if (!entries) return;
    for (struct dirent *entry; (entry = readdir(entries));) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (name[0] == '.') continue;
        char *full = xmalloc(strlen(folder) + len + 2);
        sprintf(full, "%s/%s", folder, name);
        struct stat info;
        bool is_file = false;
        if (stat(full, &info) == 0 && S_ISDIR(info.st_mode)) {
            collect_wind_files(full, list, depth + 1);
        } else if (stat(full, &info) == 0 && S_ISREG(info.st_mode) && len > 5 &&
                   strcmp(name + len - 5, ".wind") == 0) {
            is_file = true;
        }
        if (!is_file) {
            free(full);
            continue;
        }
        if (list->count == list->cap) {
            list->cap = list->cap ? list->cap * 2 : 8;
            list->items = xrealloc(list->items, list->cap * sizeof *list->items);
        }
        list->items[list->count++] = full;
    }
    closedir(entries);
}

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t wind_files_in(const char *folder, char ***files, bool *too_large) {
    PathList list = {0};
    collect_wind_files(folder, &list, 0);
    if (too_large) *too_large = list.too_large;
    if (list.too_large) {
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items);
        *files = NULL;
        return 0;
    }
    if (list.count) qsort(list.items, list.count, sizeof *list.items, compare_paths);
    *files = list.items;
    return list.count;
}
