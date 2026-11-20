/* The target for boards programmed with the Arduino toolchain: those that
   have an Arduino core, which arduino-cli or the Arduino IDE builds for,
   such as an Arduino Uno or a Raspberry Pi Pico. The generated C is one
   file of a sketch; the other is arduino_sketch, a fixed C++ file that
   starts the program from setup() and loop() and lends it the board's
   serial port. Output, input and error messages all go through that port,
   and a program that stops after an error waits there for the board to be
   reset.

   Sizes are those of the AVR boards (the Uno, Nano and Mega), on which a
   pointer is 16 bits and a double is the same as a float. */

#define _DEFAULT_SOURCE /* usleep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include "../json.h"
#include "target.h"

/* The float writer that the ones below share. It works digit by digit, so
   that it needs nothing from a C library that a board may lack. */
#define WRITE_DOUBLE                                                            \
    "/* Writes v with up to `digits` significant digits, as C's \"%g\" would,\n" \
    "   but without the zeros that end a fraction. */\n"                         \
    "static void wind_write_double(double v, int digits) {\n"                     \
    "    char text[40];\n"                                                       \
    "    char d[20];\n"                                                          \
    "    int n = 0;\n"                                                           \
    "    int exponent = 0;\n"                                                    \
    "    if (v != v) {\n"                                                        \
    "        wind_put(\"nan\", 3);\n"                                            \
    "        return;\n"                                                          \
    "    }\n"                                                                    \
    "    if (v < 0.0 || (v == 0.0 && 1.0 / v < 0.0)) {\n"                         \
    "        wind_put(\"-\", 1);\n"                                              \
    "        v = -v;\n"                                                          \
    "    }\n"                                                                    \
    "    if (v - v != 0.0) {\n"                                                  \
    "        wind_put(\"inf\", 3);\n"                                            \
    "        return;\n"                                                          \
    "    }\n"                                                                    \
    "    if (v == 0.0) {\n"                                                      \
    "        wind_put(\"0\", 1);\n"                                              \
    "        return;\n"                                                          \
    "    }\n"                                                                    \
    "    while (v >= 10.0) {\n"                                                  \
    "        v /= 10.0;\n"                                                       \
    "        exponent++;\n"                                                      \
    "    }\n"                                                                    \
    "    while (v < 1.0) {\n"                                                    \
    "        v *= 10.0;\n"                                                       \
    "        exponent--;\n"                                                      \
    "    }\n"                                                                    \
    "    for (int i = 0; i < digits; i++) {\n"                                   \
    "        int digit = (int)v;\n"                                              \
    "        if (digit > 9) digit = 9;\n"                                        \
    "        d[i] = (char)('0' + digit);\n"                                      \
    "        v = (v - digit) * 10.0;\n"                                          \
    "    }\n"                                                                    \
    "    if (v >= 5.0) {\n"                                                      \
    "        int i = digits - 1;\n"                                              \
    "        while (i >= 0 && d[i] == '9') d[i--] = '0';\n"                      \
    "        if (i >= 0) {\n"                                                    \
    "            d[i]++;\n"                                                      \
    "        } else {\n"                                                         \
    "            d[0] = '1';\n"                                                  \
    "            exponent++;\n"                                                  \
    "        }\n"                                                                \
    "    }\n"                                                                    \
    "    int used = digits;\n"                                                   \
    "    while (used > 1 && d[used - 1] == '0') used--;\n"                       \
    "    if (exponent < -4 || exponent >= digits) {\n"                           \
    "        int e = exponent < 0 ? -exponent : exponent;\n"                     \
    "        text[n++] = d[0];\n"                                                \
    "        if (used > 1) text[n++] = '.';\n"                                   \
    "        for (int i = 1; i < used; i++) text[n++] = d[i];\n"                 \
    "        text[n++] = 'e';\n"                                                 \
    "        text[n++] = exponent < 0 ? '-' : '+';\n"                            \
    "        if (e >= 100) text[n++] = (char)('0' + e / 100);\n"                 \
    "        text[n++] = (char)('0' + e / 10 % 10);\n"                           \
    "        text[n++] = (char)('0' + e % 10);\n"                                \
    "    } else if (exponent >= 0) {\n"                                          \
    "        for (int i = 0; i <= exponent; i++) text[n++] = i < used ? d[i] : '0';\n" \
    "        if (used > exponent + 1) text[n++] = '.';\n"                        \
    "        for (int i = exponent + 1; i < used; i++) text[n++] = d[i];\n"      \
    "    } else {\n"                                                             \
    "        text[n++] = '0';\n"                                                 \
    "        text[n++] = '.';\n"                                                 \
    "        for (int i = -1; i > exponent; i--) text[n++] = '0';\n"             \
    "        for (int i = 0; i < used; i++) text[n++] = d[i];\n"                 \
    "    }\n"                                                                    \
    "    wind_put(text, (size_t)n);\n"                                           \
    "}\n"

/* Significant digits for a double: six where it is 32 bits, as on the AVR
   boards, and fifteen where it is 64. */
#define DOUBLE_DIGITS "(sizeof(double) < 8 ? 6 : 15)"

static const HelperInfo arduino_helpers[HELPER_COUNT] = {
    [HELPER_PUT] = {
        "void wind_board_write(const char *bytes, size_t count);\n"
        "\n"
        "static void wind_put(const char *bytes, size_t count) {\n"
        "#ifdef WIND_SINK\n"
        "    if (wind_sink) {\n"
        "        wind_sink_put(bytes, count);\n"
        "        return;\n"
        "    }\n"
        "#endif\n"
        "    if (count) wind_board_write(bytes, count);\n"
        "}\n",
        HEADER_STDDEF, {0}, {0}},
    [HELPER_GET] = {
        "int wind_board_read(void);\n"
        "\n"
        "/* A byte given back by wind_unget, or -1. */\n"
        "static int wind_given_back = -1;\n"
        "\n"
        "/* Waits for the next byte to arrive. */\n"
        "static int wind_get(void) {\n"
        "    int c = wind_given_back;\n"
        "    if (c < 0) return wind_board_read();\n"
        "    wind_given_back = -1;\n"
        "    return c;\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_UNGET] = {
        "static void wind_unget(int c) {\n"
        "    wind_given_back = c;\n"
        "}\n",
        0, {HELPER_GET}, {0}},
    [HELPER_FLUSH] = {
        "/* Output leaves the board as it is written. */\n"
        "static void wind_flush(void) {\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_ERROR] = {
        "static void wind_error(const char *text) {\n"
        "    wind_put(text, strlen(text));\n"
        "}\n",
        HEADER_STRING, {HELPER_PUT}, {0}},
    [HELPER_STOP] = {
        "void wind_board_halt(void);\n"
        "\n"
        "static _Noreturn void wind_stop(void) {\n"
        "    wind_board_halt();\n"
        "    for (;;) {\n"
        "    }\n"
        "}\n",
        0, {0}, {0}},
    [HELPER_PARSE_FLOAT] = {
        "static long double wind_parse_float(const char *text, char **end) {\n"
        "    return strtod(text, end);\n"
        "}\n",
        HEADER_STDLIB, {0}, {0}},
    [HELPER_FLOAT_SUPPORT] = {WRITE_DOUBLE, HEADER_STDDEF, {HELPER_PUT}, {0}},
    [HELPER_WRITE_F16] = {
        "static void wind_write_f16(wind_f16 v) {\n"
        "    wind_write_double((double)v, 4);\n"
        "}\n",
        0, {HELPER_FLOAT_SUPPORT}, {TYPE_F16}},
    [HELPER_WRITE_F32] = {
        "static void wind_write_f32(wind_f32 v) {\n"
        "    wind_write_double((double)v, 6);\n"
        "}\n",
        0, {HELPER_FLOAT_SUPPORT}, {TYPE_F32}},
    [HELPER_WRITE_F64] = {
        "static void wind_write_f64(wind_f64 v) {\n"
        "    wind_write_double((double)v, " DOUBLE_DIGITS ");\n"
        "}\n",
        0, {HELPER_FLOAT_SUPPORT}, {TYPE_F64}},
    [HELPER_WRITE_F128] = {
        "static void wind_write_f128(wind_f128 v) {\n"
        "    wind_write_double((double)v, " DOUBLE_DIGITS ");\n"
        "}\n",
        0, {HELPER_FLOAT_SUPPORT}, {TYPE_F128}},
    [HELPER_WRITE_FIXED] = {
        "/* Writes v with digits digits after the point, rounded; one too large\n"
        "   for a 32-bit count is written as cout would write it. */\n"
        "static void wind_write_fixed(long double value, int digits) {\n"
        "    double v = (double)value;\n"
        "    if (digits < 0) digits = 0;\n"
        "    if (digits > 9) digits = 9;\n"
        "    if (v != v) {\n"
        "        wind_put(\"nan\", 3);\n"
        "        return;\n"
        "    }\n"
        "    if (v < 0.0) {\n"
        "        wind_put(\"-\", 1);\n"
        "        v = -v;\n"
        "    }\n"
        "    unsigned long scale = 1;\n"
        "    for (int i = 0; i < digits; i++) scale *= 10;\n"
        "    double scaled = v * (double)scale + 0.5;\n"
        "    if (v - v != 0.0 || scaled >= 4294967295.0) {\n"
        "        wind_write_double(v, " DOUBLE_DIGITS ");\n"
        "        return;\n"
        "    }\n"
        "    unsigned long n = (unsigned long)scaled;\n"
        "    char text[24];\n"
        "    int len = 0;\n"
        "    unsigned long whole = n / scale;\n"
        "    do {\n"
        "        text[len++] = (char)('0' + whole % 10);\n"
        "        whole /= 10;\n"
        "    } while (whole);\n"
        "    for (int i = 0, j = len - 1; i < j; i++, j--) {\n"
        "        char t = text[i];\n"
        "        text[i] = text[j];\n"
        "        text[j] = t;\n"
        "    }\n"
        "    if (digits) {\n"
        "        unsigned long part = n % scale;\n"
        "        text[len++] = '.';\n"
        "        for (int i = digits - 1; i >= 0; i--) {\n"
        "            text[len + i] = (char)('0' + part % 10);\n"
        "            part /= 10;\n"
        "        }\n"
        "        len += digits;\n"
        "    }\n"
        "    wind_put(text, (size_t)len);\n"
        "}\n",
        0, {HELPER_FLOAT_SUPPORT}, {0}},
    [HELPER_EXIT] = {
        "void wind_board_halt(void);\n"
        "\n"
        "/* A board has nowhere to return a status to, so it stops. */\n"
        "static _Noreturn void wind_exit(int status) {\n"
        "    (void)status;\n"
        "    wind_board_halt();\n"
        "    for (;;) {\n"
        "    }\n"
        "}\n",
        0, {0}, {0}},
};

/* The serial port's speed, which arduino_sketch starts it at. */
#define BAUD "9600"

/* Runs arduino-cli with the given arguments. Returns false, with the
   problem reported, if it fails. */
static bool arduino_cli(char *const argv[], const char *what) {
    int code = run_program("arduino-cli", argv);
    if (code == 0) return true;
    fprintf(stderr, "wind: error: arduino-cli could not %s the sketch (exit code %d)\n", what,
            code);
    return false;
}

/* The port that the one connected board arduino-cli recognizes as `board`
   is on, from malloc. Returns NULL, with the problem reported, if there is
   no such board or more than one. */
static char *find_port(const char *board) {
    char *list[] = {"arduino-cli", "board", "list", "--format", "json", NULL};
    int status;
    char *text = program_output("arduino-cli", list, &status);
    Arena arena = {0};
    const Json *found = status == 0 ? json_parse(&arena, (Str){text, strlen(text)}) : NULL;
    const Json *ports = json_get(found, "detected_ports");
    char *port = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < json_count(ports); i++) {
        const Json *detected = json_at(ports, i);
        const Json *boards = json_get(detected, "matching_boards");
        Str address;
        if (!json_string(json_get(json_get(detected, "port"), "address"), &address)) continue;
        for (size_t j = 0; j < json_count(boards); j++) {
            Str fqbn;
            if (!json_string(json_get(json_at(boards, j), "fqbn"), &fqbn)) continue;
            if (!str_eq_cstr(fqbn, board)) continue;
            if (matches++ == 0) {
                port = xmalloc(address.len + 1);
                memcpy(port, address.data, address.len);
                port[address.len] = '\0';
            }
            break;
        }
    }
    arena_release(&arena);
    free(text);
    if (status != 0 || !found) {
        fprintf(stderr, "wind: error: arduino-cli could not list the connected boards; give the "
                        "port with -p\n");
    } else if (matches == 0) {
        fprintf(stderr,
                "wind: error: no board '%s' is connected; connect one, or give its port with "
                "-p ('arduino-cli board list' shows what is connected)\n",
                board);
    } else if (matches > 1) {
        fprintf(stderr, "wind: error: %zu boards '%s' are connected; choose one with -p\n",
                matches, board);
    }
    if (status != 0 || !found || matches != 1) {
        free(port);
        return NULL;
    }
    return port;
}

/* Finds a program other than this one that has the port open, where the
   system shows that in /proc. Returns its process id and writes its name
   to name, or returns 0 if there is none or it cannot be told. */
static long port_holder(const char *port, char *name, size_t size) {
    char *device = path_canonical(port);
    DIR *processes = device ? opendir("/proc") : NULL;
    long found = 0;
    for (struct dirent *entry; processes && !found && (entry = readdir(processes));) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid == (long)getpid()) continue;
        char dir_path[64];
        snprintf(dir_path, sizeof dir_path, "/proc/%ld/fd", pid);
        DIR *files = opendir(dir_path);
        for (struct dirent *file; files && !found && (file = readdir(files));) {
            char link[400], target[4096];
            snprintf(link, sizeof link, "%s/%s", dir_path, file->d_name);
            ssize_t len = readlink(link, target, sizeof target - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            if (strcmp(target, device) == 0) found = pid;
        }
        if (files) closedir(files);
    }
    if (processes) closedir(processes);
    free(device);
    name[0] = '\0';
    if (found) {
        char comm_path[64];
        snprintf(comm_path, sizeof comm_path, "/proc/%ld/comm", found);
        FILE *comm = fopen(comm_path, "r");
        if (comm && fgets(name, (int)size, comm)) name[strcspn(name, "\n")] = '\0';
        if (comm) fclose(comm);
    }
    return found;
}

static int arduino_run(const char *folder, const char *board, const char *given_port) {
    if (!program_on_path("arduino-cli")) {
        fprintf(stderr,
                "wind: error: running a program for this target uses arduino-cli, which is "
                "not in PATH; install it from https://arduino.github.io/arduino-cli/, then "
                "the core for the board with 'arduino-cli core install'\n");
        return 1;
    }
    char *found_port = given_port ? NULL : find_port(board);
    if (!given_port && !found_port) return 1;
    const char *port = given_port ? given_port : found_port;

    /* A board that is restarted for an upload through its port, as one that
       talks over USB is, cannot be while another program holds the port. */
    char holder[64];
    long pid = port_holder(port, holder, sizeof holder);
    if (pid) {
        fprintf(stderr,
                "wind: error: %s is open in another program (%s, process %ld), so the board "
                "cannot be restarted to take the upload; close it, such as the Serial "
                "Monitor of the Arduino IDE, and run again\n",
                port, holder[0] ? holder : "unknown", pid);
        free(found_port);
        return 1;
    }
    char *compile[] = {"arduino-cli", "compile", "--fqbn", (char *)board, (char *)folder,
                       NULL};
    char *upload[] = {"arduino-cli", "upload", "--fqbn", (char *)board, "-p", (char *)port,
                      (char *)folder, NULL};
    if (!arduino_cli(compile, "compile") || !arduino_cli(upload, "upload")) {
        free(found_port);
        return 1;
    }

    /* A board that talks over USB restarts after an upload and its port
       goes away for a moment, so it is given a second and then waited for,
       for up to ten more. */
    usleep(1000000);
    for (int tries = 0; tries < 100 && access(port, R_OK | W_OK) != 0; tries++) usleep(100000);
    char *monitor[] = {"arduino-cli", "monitor", "-p", (char *)port, "--config",
                       "baudrate=" BAUD, NULL};
    int code = run_program("arduino-cli", monitor);
    free(found_port);
    return code;
}

const Target target_arduino = {
    .platform = {
        .name = "arduino",
        .pointer_size = 2,
        .double_size = 4,
        .long_double_size = 4,
        .wide_integers = false,
        .has_arguments = false,
        .repeat_function = "loop",
    },
    .helpers = arduino_helpers,
    .start = "void wind_setup(void)",
    .start_with_arguments = NULL,
    .start_returns_status = false,
    .repeat = "void wind_loop(void)",
    /* Wind defines signed arithmetic as wrapping, and lets a pointer read
       memory as another type; a sketch is not compiled with the flags that
       make C agree, so the file asks for them itself. */
    .prologue = "#pragma GCC optimize (\"wrapv\")\n"
                "#pragma GCC optimize (\"no-strict-aliasing\")\n\n",
    .help =
        "Target arduino: a program for a board programmed with the Arduino\n"
        "toolchain, which is any board that has an Arduino core, such as an\n"
        "Arduino Uno or a Raspberry Pi Pico.\n"
        "\n"
        "  wind build -t arduino <file.wind> [-o <folder>]\n"
        "      write a sketch folder holding <folder>.ino and wind_program.c; the\n"
        "      folder is named after the file unless -o names another\n"
        "  wind run -t arduino [--fqbn <board>] [-p <port>] <file.wind>\n"
        "      build the sketch, compile and upload it with arduino-cli, and show\n"
        "      what the program prints until Ctrl-C\n"
        "  wind check -t arduino <file.wind>...\n"
        "      report errors and warnings, with the board's sizes and rules\n"
        "  wind emit -t arduino <file.wind>\n"
        "      print the C in wind_program.c\n"
        "\n"
        "A file can say all this at its top, so that the options are not needed:\n"
        "  #target arduino\n"
        "  #board rp2040:rp2040:rpipico2w\n"
        "\n"
        "For run, the board is named as arduino-cli names it, with #board or\n"
        "--fqbn, and is looked for among the connected boards. -p (or --port)\n"
        "gives its port, such as /dev/ttyACM0, when it is not found or when\n"
        "several are; 'arduino-cli board list' shows both. arduino-cli must be\n"
        "installed, with the core for the board.\n"
        "\n"
        "The program:\n"
        "  - Its top level, or its main, runs once when the board starts.\n"
        "  - A function 'void loop()' then runs over and over.\n"
        "  - cout, cin and error messages use the serial port, at 9600 baud.\n"
        "  - An index out of range or a NULL pointer that is used prints its\n"
        "    message and stops the program until the board is reset.\n"
        "  - main takes no arguments.\n"
        "\n"
        "Sizes are those of the AVR boards (Uno, Nano, Mega): int, uint, uintptr\n"
        "and pointers are 16 bits, float and f64 are 32 bits, and i128 and u128\n"
        "are not available.\n"
        "\n"
        "Libraries:\n"
        "  import ::vendor/arduino    pins, time and the board's constants\n"
        "\n"
        "Running a program on an Arduino Uno:\n"
        "  wind run -t arduino --fqbn arduino:avr:uno blink.wind\n",
    .run = arduino_run,
};

const char arduino_sketch[] =
    "// Starts a Wind program on the board. The program itself is in\n"
    "// wind_program.c, written by \"wind build -t arduino\"; this file runs\n"
    "// the program's top level once from setup() and its loop function from\n"
    "// loop(), and lends it the serial port for output, input and error\n"
    "// messages.\n"
    "\n"
    "#include <Arduino.h>\n"
    "\n"
    "extern \"C\" {\n"
    "void wind_setup(void);\n"
    "void wind_loop(void);\n"
    "\n"
    "void wind_board_write(const char *bytes, size_t count) {\n"
    "    Serial.write((const uint8_t *)bytes, count);\n"
    "}\n"
    "\n"
    "// Waits for the next byte to arrive.\n"
    "int wind_board_read(void) {\n"
    "    while (Serial.available() <= 0) yield();\n"
    "    return Serial.read();\n"
    "}\n"
    "\n"
    "// Whether a computer has the serial port open, and how many bytes have\n"
    "// arrived on it and not yet been read. The library's serial.wind\n"
    "// declares these.\n"
    "int wind_serial_connected(void) {\n"
    "    return Serial ? 1 : 0;\n"
    "}\n"
    "\n"
    "int wind_serial_available(void) {\n"
    "    return Serial.available();\n"
    "}\n"
    "\n"
    "// Stops the program after an error, leaving the board running so that\n"
    "// the message is sent and the board can be reset.\n"
    "void wind_board_halt(void) {\n"
    "    Serial.flush();\n"
    "    for (;;) delay(1000);\n"
    "}\n"
    "}\n"
    "\n"
    "void setup() {\n"
    "    Serial.begin(" BAUD ");\n"
    "    wind_setup();\n"
    "}\n"
    "\n"
    "void loop() {\n"
    "    wind_loop();\n"
    "}\n";
