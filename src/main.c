#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L /* open_memstream */

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
#include "targets/target.h"

static void usage(FILE *out) {
    fputs("usage:\n"
          "  wind build <file.wind> [-o <output>]   compile to an executable, or for\n"
          "                                         -t arduino to a sketch folder\n"
          "  wind run <file.wind> [args...]         compile and run; for -t arduino,\n"
          "                                         on a board (see wind help -t arduino)\n"
          "  wind <file.wind> [args...]             same as run\n"
          "  wind check <file.wind>...              report errors and warnings\n"
          "  wind emit <file.wind>                  print the generated C\n"
          "  wind lsp                               run the language server on stdio\n"
          "  wind version | -v | --version          print the version\n"
          "  wind help | -h | --help                show this help\n"
          "\n"
          "options for build, run, check and emit (for run, before the file):\n"
          "  -t, --target <name>   what to compile for: c or arduino; without this\n"
          "                        option, what the file's #target says, else c\n"
          "  --no-bounds-check     leave out the test that stops a program when an\n"
          "                        index is out of range\n"
          "\n"
          "A file can name its target at its top with '#target <name>', which -t\n"
          "overrides. wind help -t <name> describes a target.\n"
          "\n"
          "An import that starts with \"::\" is found in the libraries directory:\n"
          "$WIND_LIBS if it is set, else the one installed with wind.\n"
          "\n"
          "The C compiler is taken from $CC (default: cc).\n",
          out);
}

static void print_diags(const Unit *unit) {
    for (size_t i = 0; i < unit->diags.count; i++) {
        const Diag *diag = &unit->diags.items[i];
        fprintf(stderr, "%s:%d:%d: %s: %s\n", unit->path, diag->loc.line,
                diag->loc.col, severity_name(diag->severity), diag->message);
    }
}

/* Analyzes a file and prints its diagnostics to stderr, one per line as
   "file:line:col: severity: message": those of the files it imports first,
   then its own. */
static void analyze_file(Unit *unit, const char *src_path) {
    Str src;
    if (!file_read(src_path, &src)) fatal("cannot read '%s': %s", src_path, strerror(errno));
    unit_analyze(unit, src, src_path);
    for (size_t i = 0; i < unit->module_count; i++) print_diags(unit->modules[i]);
    print_diags(unit);
}

/* Exits with status 1 if the file has errors; warnings do not stop it. */
static void compile_to_c(const char *src_path, FILE *out, EmitOptions options) {
    Unit unit;
    analyze_file(&unit, src_path);
    if (unit.diags.error_count > 0) exit(1);
    emit_c(&unit, out, options);
    free((char *)unit.src.data);
    unit_release(&unit);
}

static void build(const char *src_path, const char *out_path, EmitOptions options) {
    char c_path[4096];
    snprintf(c_path, sizeof c_path, "%s/wind-XXXXXX.c", temp_dir());
    int fd = mkstemps(c_path, 2);
    if (fd < 0) fatal("cannot create '%s': %s", c_path, strerror(errno));
    FILE *c_file = fdopen(fd, "w");
    if (!c_file) fatal("cannot open '%s': %s", c_path, strerror(errno));

    compile_to_c(src_path, c_file, options);
    if (fclose(c_file) != 0) {
        fatal("cannot write '%s': %s", c_path, strerror(errno));
    }

    /* -fwrapv makes signed overflow wrap, as Wind defines it.
       -fno-strict-aliasing lets a pointer converted to another pointer type
       read the bytes it finds. -w hides C warnings, which would describe
       generated code the user never wrote. Then come the C libraries that
       the program's external functions say to link. */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    size_t link_count;
    const char *const *link_names = emit_c_links(&link_count);
    char **argv = xmalloc((10 + link_count) * sizeof *argv);
    size_t argc = 0;
    const char *fixed[] = {cc, "-std=c11", "-O2", "-w", "-fwrapv", "-fno-strict-aliasing",
                           "-o", out_path, c_path};
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) argv[argc++] = (char *)fixed[i];
    for (size_t i = 0; i < link_count; i++) {
        char *flag = xmalloc(strlen(link_names[i]) + 3);
        sprintf(flag, "-l%s", link_names[i]);
        argv[argc++] = flag;
    }
    argv[argc] = NULL;
    int code = run_program(cc, argv);
    for (size_t i = 9; i < argc; i++) free(argv[i]);
    free(argv);
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

/* The target a name given with -t or --target stands for. */
static const Target *target_named(const char *name) {
    const Target *target = target_find((Str){name, strlen(name)});
    if (!target) fatal("unknown target '%s'; the targets are " TARGET_NAMES, name);
    return target;
}

/* If argv[*i] is an option that every command takes, applies it to
   *options, moves *i past any value it has, and returns true:
   --no-bounds-check, and -t or --target with the target's name, which may
   also be written --target=name. */
static bool common_option(int argc, char **argv, int *i, EmitOptions *options) {
    const char *arg = argv[*i];
    if (strcmp(arg, "--no-bounds-check") == 0) {
        options->no_bounds_check = true;
        return true;
    }
    if (strncmp(arg, "--target=", 9) == 0) {
        options->target = target_named(arg + 9);
        return true;
    }
    if (strcmp(arg, "-t") == 0 || strcmp(arg, "--target") == 0) {
        if (++*i == argc) fatal("'%s' requires the name of a target", arg);
        options->target = target_named(argv[*i]);
        return true;
    }
    return false;
}

/* What the file at path says about what it is compiled for. */
static FileSettings settings_of(const char *path) {
    Str src;
    if (!file_read(path, &src)) fatal("cannot read '%s': %s", path, strerror(errno));
    FileSettings settings = file_settings(src);
    free((char *)src.data);
    return settings;
}

/* Settles what the program at path is compiled for: the target the command
   line names, else the one the file's "#target" names, else c. A name the
   file gets wrong is reported when the file is checked. Checking depends
   on the target, so its platform is selected here, before the file is. */
static const Target *use_target(EmitOptions *options, const FileSettings *settings) {
    if (!options->target && settings->target) {
        options->target = target_find((Str){settings->target, strlen(settings->target)});
    }
    if (!options->target) options->target = &target_host;
    types_use_platform(&options->target->platform);
    return options->target;
}

/* The name a sketch folder has: its last part. */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* Creates a directory and any of its parents that are missing. */
static void make_directories(const char *path) {
    char *partial = xmalloc(strlen(path) + 1);
    strcpy(partial, path);
    for (char *at = partial + 1;; at++) {
        bool end = *at == '\0';
        if (*at != '/' && !end) continue;
        *at = '\0';
        if (mkdir(partial, 0777) != 0 && errno != EEXIST) {
            fatal("cannot create '%s': %s", partial, strerror(errno));
        }
        if (end) break;
        *at = '/';
    }
    free(partial);
}

static void write_file(const char *path, const char *text, size_t len) {
    FILE *file = fopen(path, "w");
    if (!file) fatal("cannot create '%s': %s", path, strerror(errno));
    if (fwrite(text, 1, len, file) != len || fclose(file) != 0) {
        fatal("cannot write '%s': %s", path, strerror(errno));
    }
}

/* Writes an Arduino sketch: a folder holding the program as wind_program.c
   and the sketch file that starts it, named after the folder as Arduino
   requires. */
static void build_sketch(const char *src_path, const char *folder, EmitOptions options,
                         bool quiet) {
    /* The program is generated before anything is written, so that a
       program with errors leaves no files behind. */
    char *program = NULL;
    size_t program_len = 0;
    FILE *buffer = open_memstream(&program, &program_len);
    if (!buffer) fatal("out of memory");
    compile_to_c(src_path, buffer, options);
    if (fclose(buffer) != 0) fatal("out of memory");

    make_directories(folder);
    const char *name = base_name(folder);
    char *sketch_path = xmalloc(strlen(folder) + strlen(name) + 6);
    char *program_path = xmalloc(strlen(folder) + sizeof "/wind_program.c");
    sprintf(sketch_path, "%s/%s.ino", folder, name);
    sprintf(program_path, "%s/wind_program.c", folder);
    write_file(sketch_path, arduino_sketch, strlen(arduino_sketch));
    write_file(program_path, program, program_len);
    if (!quiet) printf("wrote %s and %s\n", sketch_path, program_path);
    free(sketch_path);
    free(program_path);
    free(program);
}

/* Builds what the target runs: an executable for c, a sketch folder for
   arduino. Either is named after the source file, in the current
   directory, unless -o gives another name. */
static int cmd_build(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    EmitOptions options = {0};
    for (int i = 0; i < argc; i++) {
        if (common_option(argc, argv, &i, &options)) {
            /* applied */
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) fatal("'-o' requires an argument");
            out_path = argv[i];
        } else if (!src_path) {
            src_path = argv[i];
        } else {
            fatal("unexpected argument '%s'", argv[i]);
        }
    }
    if (!src_path) fatal("no input file");
    FileSettings settings = settings_of(src_path);
    const Target *target = use_target(&options, &settings);
    file_settings_release(&settings);
    char *named = out_path ? NULL : default_output(src_path);
    if (target == &target_arduino) {
        /* A folder needs no ".out" to keep it apart from the source. */
        size_t len = named ? strlen(named) : 0;
        if (len > 4 && strcmp(named + len - 4, ".out") == 0) named[len - 4] = '\0';
        build_sketch(src_path, out_path ? out_path : named, options, false);
    } else {
        build(src_path, out_path ? out_path : named, options);
    }
    free(named);
    return 0;
}

/* Runs a program built for a target whose programs run on a board: the
   sketch goes in a temporary folder, which the target compiles, uploads and
   watches, and which is removed afterwards. */
static int run_on_board(const char *src_path, EmitOptions options, const char *board,
                        const char *port) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/wind-XXXXXX", temp_dir());
    if (!mkdtemp(dir)) fatal("cannot create '%s': %s", dir, strerror(errno));
    /* Arduino names a sketch after its folder, and allows letters, digits,
       "_", "-" and "." there. */
    char *name = default_output(src_path);
    for (char *at = name; *at; at++) {
        bool allowed = (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') ||
                       (*at >= '0' && *at <= '9') || *at == '_' || *at == '-' || *at == '.';
        if (!allowed) *at = '_';
    }
    char *folder = xmalloc(strlen(dir) + strlen(name) + 2);
    sprintf(folder, "%s/%s", dir, name);
    build_sketch(src_path, folder, options, true);

    int code = options.target->run(folder, board, port);

    char *ino = xmalloc(strlen(folder) + strlen(name) + 6);
    char *program = xmalloc(strlen(folder) + sizeof "/wind_program.c");
    sprintf(ino, "%s/%s.ino", folder, name);
    sprintf(program, "%s/wind_program.c", folder);
    unlink(ino);
    unlink(program);
    rmdir(folder);
    rmdir(dir);
    free(ino);
    free(program);
    free(folder);
    free(name);
    return code;
}

/* Builds and runs a program: here, or for a target whose programs run on
   a board, on the board that --fqbn or the file's #board names, through
   the port -p names or else the one that board is found on. The options
   come before the file; what follows the file is the program's own
   arguments. */
static int cmd_run(int argc, char **argv) {
    EmitOptions options = {0};
    const char *board = NULL;
    const char *port = NULL;
    int first = 0;
    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (common_option(argc, argv, &first, &options)) continue;
        const char **value = strcmp(arg, "--fqbn") == 0 ? &board
                             : strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0 ? &port
                                                                                    : NULL;
        if (!value) break;
        if (++first == argc) fatal("'%s' requires an argument", arg);
        *value = argv[first];
    }
    argc -= first;
    argv += first;
    if (argc < 1) fatal("no input file");
    FileSettings settings = settings_of(argv[0]);
    const Target *target = use_target(&options, &settings);
    const char *name = target->platform.name;
    if (target->run) {
        /* The board the command line names, else the file's. */
        if (!board) board = settings.board;
        if (!board) {
            fatal("running a program for %s needs the board's name: give the file '#board "
                  "arduino:avr:uno' at its top, or give 'wind run' '--fqbn "
                  "arduino:avr:uno'; 'arduino-cli board list' shows the names",
                  name);
        }
        if (argc > 1) fatal("a program for %s takes no arguments", name);
        int code = run_on_board(argv[0], options, board, port);
        file_settings_release(&settings);
        return code;
    }
    file_settings_release(&settings);
    if (board || port) {
        fatal("--fqbn and -p name a board, which a program for %s does not run on", name);
    }

    char exe_path[4096];
    snprintf(exe_path, sizeof exe_path, "%s/wind-XXXXXX", temp_dir());
    int fd = mkstemp(exe_path);
    if (fd < 0) fatal("cannot create '%s': %s", exe_path, strerror(errno));
    close(fd);

    build(argv[0], exe_path, options);
    chmod(exe_path, 0700);

    /* The program's argv[0] is the source file it was built from, followed
       by the caller's extra arguments; argv is NULL-terminated, so the tail
       carries over. */
    int code = run_program(exe_path, argv);
    unlink(exe_path);
    return code;
}

static int cmd_check(int argc, char **argv) {
    EmitOptions options = {0};
    int count = 0;
    for (int i = 0; i < argc; i++) {
        if (!common_option(argc, argv, &i, &options)) argv[count++] = argv[i];
    }
    if (count < 1) fatal("no input file");
    bool failed = false;
    for (int i = 0; i < count; i++) {
        /* Each file says its own target, unless the command line names one. */
        EmitOptions for_file = options;
        FileSettings settings = settings_of(argv[i]);
        use_target(&for_file, &settings);
        file_settings_release(&settings);
        Unit unit;
        analyze_file(&unit, argv[i]);
        if (unit.diags.error_count > 0) failed = true;
        free((char *)unit.src.data);
        unit_release(&unit);
    }
    return failed ? 1 : 0;
}

/* Prints the C that a program becomes for the target. For arduino that is
   the sketch's wind_program.c; its .ino file is the same for every
   program. */
static int cmd_emit(int argc, char **argv) {
    EmitOptions options = {0};
    const char *src_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (common_option(argc, argv, &i, &options)) continue;
        if (src_path) fatal("emit takes exactly one input file");
        src_path = argv[i];
    }
    if (!src_path) fatal("no input file");
    FileSettings settings = settings_of(src_path);
    use_target(&options, &settings);
    file_settings_release(&settings);
    compile_to_c(src_path, stdout, options);
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

/* Handles a request for help: a help spelling and at most a target
   option, in either order, as in "wind -h -t arduino". Prints the help it
   asks for, the target's if one is named, and returns true. Returns false,
   doing nothing, when the first argument is neither a help spelling nor a
   target option, so that a program's own arguments are left alone. */
static bool help_request(int argc, char **argv) {
    const char *first = argv[1];
    bool starts_with_target = strcmp(first, "-t") == 0 || strcmp(first, "--target") == 0 ||
                              strncmp(first, "--target=", 9) == 0;
    if (!is_one_of(first, help_spellings) && !starts_with_target) return false;
    bool asked = false;
    for (int i = 1; i < argc; i++) asked = asked || is_one_of(argv[i], help_spellings);
    if (!asked) {
        fatal("the target goes after the command, as in 'wind build -t <name> file.wind'");
    }
    EmitOptions options = {0};
    for (int i = 1; i < argc; i++) {
        if (is_one_of(argv[i], help_spellings)) {
            /* already noted */
        } else if (strcmp(argv[i], "--no-bounds-check") == 0 ||
                   !common_option(argc, argv, &i, &options)) {
            fatal("unexpected argument '%s'; help takes only a target, as in "
                  "'wind help -t arduino'",
                  argv[i]);
        }
    }
    if (options.target) {
        fputs(options.target->help, stdout);
    } else {
        usage(stdout);
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    libs_locate(argv[0]);
    if (help_request(argc, argv)) return 0;
    const char *cmd = argv[1];
    if (strcmp(cmd, "build") == 0) return cmd_build(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(cmd, "lsp") == 0) return lsp_run();
    if (strcmp(cmd, "emit") == 0) return cmd_emit(argc - 2, argv + 2);
    if (is_one_of(cmd, version_spellings)) {
        puts("wind " WIND_VERSION);
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
