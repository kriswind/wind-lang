/* The target for Arduino boards. The generated C is one file of a sketch;
   the other is arduino_sketch, a fixed C++ file that starts the program from
   setup() and loop() and lends it the board's serial port. Output, input and
   error messages all go through that port, and a program that stops after an
   error waits there for the board to be reset.

   Sizes are those of the AVR boards (the Uno, Nano and Mega), on which a
   pointer is 16 bits and a double is the same as a float. */

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
};

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
};

const char arduino_sketch[] =
    "// Starts a Wind program on the board. The program itself is in\n"
    "// wind_program.c, written by \"wind emit-arduino\"; this file runs its top\n"
    "// level once from setup() and its loop function from loop(), and lends it\n"
    "// the serial port for output, input and error messages.\n"
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
    "// Stops the program after an error, leaving the board running so that\n"
    "// the message is sent and the board can be reset.\n"
    "void wind_board_halt(void) {\n"
    "    Serial.flush();\n"
    "    for (;;) delay(1000);\n"
    "}\n"
    "}\n"
    "\n"
    "void setup() {\n"
    "    Serial.begin(9600);\n"
    "    wind_setup();\n"
    "}\n"
    "\n"
    "void loop() {\n"
    "    wind_loop();\n"
    "}\n";
