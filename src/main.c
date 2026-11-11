#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ast.h"
#include "codegen.h"
#include "common.h"
#include "lsp.h"

static void usage(FILE *out) {
    fputs("usage:\n"
          "  wind build <file.wind> [-o <output>]   compile to an executable\n"
          "  wind run <file.wind> [args...]         compile and run\n"
          "  wind <file.wind> [args...]             same as run\n"
          "  wind check <file.wind>...              report errors and warnings\n"
          "  wind lsp                               run the language server on stdio\n"
          "  wind emit-c <file.wind>                print the generated C\n"
          "  wind version | -v | --version          print the version\n"
          "  wind help | -h | --help                show this help\n"
          "\n"
          "The C compiler is taken from $CC (default: cc).\n",
          out);
}

static Str read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open '%s': %s", path, strerror(errno));

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
    if (ferror(f)) fatal("cannot read '%s': %s", path, strerror(errno));
    fclose(f);
    return (Str){buf, len};
}

/* Analyzes a file and prints its diagnostics to stderr, one per line as
   "file:line:col: severity: message". */
static void analyze_file(Unit *unit, const char *src_path) {
    unit_analyze(unit, read_file(src_path));
    for (size_t i = 0; i < unit->diags.count; i++) {
        const Diag *diag = &unit->diags.items[i];
        fprintf(stderr, "%s:%d:%d: %s: %s\n", src_path, diag->loc.line,
                diag->loc.col, severity_name(diag->severity), diag->message);
    }
}

/* Exits with status 1 if the file has errors; warnings do not stop it. */
static void compile_to_c(const char *src_path, FILE *out) {
    Unit unit;
    analyze_file(&unit, src_path);
    if (unit.diags.error_count > 0) exit(1);
    emit_c(&unit, out);
    free((char *)unit.src.data);
    unit_release(&unit);
}

static const char *temp_dir(void) {
    const char *dir = getenv("TMPDIR");
    return (dir && *dir) ? dir : "/tmp";
}

/* Returns the child's exit code, or 128 + signal number if it was killed. */
static int spawn_and_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) fatal("fork failed: %s", strerror(errno));
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "wind: error: cannot execute '%s': %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        fatal("waitpid failed: %s", strerror(errno));
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

static void build(const char *src_path, const char *out_path) {
    char c_path[4096];
    snprintf(c_path, sizeof c_path, "%s/wind-XXXXXX.c", temp_dir());
    int fd = mkstemps(c_path, 2);
    if (fd < 0) fatal("cannot create '%s': %s", c_path, strerror(errno));
    FILE *c_file = fdopen(fd, "w");
    if (!c_file) fatal("cannot open '%s': %s", c_path, strerror(errno));

    compile_to_c(src_path, c_file);
    if (fclose(c_file) != 0) {
        fatal("cannot write '%s': %s", c_path, strerror(errno));
    }

    /* -fwrapv makes signed overflow wrap, as Wind defines it. -w hides C
       warnings, which would describe generated code the user never wrote. */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    char *argv[] = {
        (char *)cc, "-std=c11", "-O2", "-w", "-fwrapv", "-o", (char *)out_path,
        c_path,     NULL,
    };
    int code = spawn_and_wait(argv);
    unlink(c_path);
    if (code != 0) fatal("C compiler '%s' failed (exit code %d)", cc, code);
}

/* "dir/hello.wind" -> "hello"; a name without an extension gets ".out" so the
   output never overwrites the source. */
static char *default_output(const char *src_path) {
    const char *slash = strrchr(src_path, '/');
    const char *base = slash ? slash + 1 : src_path;
    char *out = xmalloc(strlen(base) + sizeof ".out");
    strcpy(out, base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    } else {
        strcat(out, ".out");
    }
    return out;
}

static int cmd_build(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) fatal("'-o' requires an argument");
            out_path = argv[i];
        } else if (!src_path) {
            src_path = argv[i];
        } else {
            fatal("unexpected argument '%s'", argv[i]);
        }
    }
    if (!src_path) fatal("no input file");
    build(src_path, out_path ? out_path : default_output(src_path));
    return 0;
}

static int cmd_run(int argc, char **argv) {
    if (argc < 1) fatal("no input file");

    char exe_path[4096];
    snprintf(exe_path, sizeof exe_path, "%s/wind-XXXXXX", temp_dir());
    int fd = mkstemp(exe_path);
    if (fd < 0) fatal("cannot create '%s': %s", exe_path, strerror(errno));
    close(fd);

    build(argv[0], exe_path);
    chmod(exe_path, 0700);

    /* The program sees its own path as argv[0], followed by the caller's
       extra arguments; argv is NULL-terminated, so the tail carries over. */
    argv[0] = exe_path;
    int code = spawn_and_wait(argv);
    unlink(exe_path);
    return code;
}

static int cmd_check(int argc, char **argv) {
    if (argc < 1) fatal("no input file");
    bool failed = false;
    for (int i = 0; i < argc; i++) {
        Unit unit;
        analyze_file(&unit, argv[i]);
        if (unit.diags.error_count > 0) failed = true;
        free((char *)unit.src.data);
        unit_release(&unit);
    }
    return failed ? 1 : 0;
}

static int cmd_emit_c(int argc, char **argv) {
    if (argc != 1) fatal("emit-c takes exactly one input file");
    compile_to_c(argv[0], stdout);
    return 0;
}

static const char *const version_spellings[] = {
    "version", "--version", "-version", "--v", "-v", "-V", NULL,
};
static const char *const help_spellings[] = {
    "help", "--help", "-help", "--h", "-h", "-?", NULL,
};

static bool is_one_of(const char *arg, const char *const *spellings) {
    for (; *spellings; spellings++) {
        if (strcmp(arg, *spellings) == 0) return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "build") == 0) return cmd_build(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(cmd, "lsp") == 0) return lsp_run();
    if (strcmp(cmd, "emit-c") == 0) return cmd_emit_c(argc - 2, argv + 2);
    if (is_one_of(cmd, version_spellings)) {
        puts("wind " WIND_VERSION);
        return 0;
    }
    if (is_one_of(cmd, help_spellings)) {
        usage(stdout);
        return 0;
    }
    /* Anything that is not a command is taken as a program to run, if it
       names a Wind source file or an existing file. */
    size_t len = strlen(cmd);
    bool is_source = len > 5 && strcmp(cmd + len - 5, ".wind") == 0;
    if (is_source || access(cmd, F_OK) == 0) return cmd_run(argc - 1, argv + 1);

    fprintf(stderr, "wind: error: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return 2;
}
