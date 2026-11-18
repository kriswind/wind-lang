/* The target for the machine the compiler runs on: a program reads stdin,
   writes stdout and stderr, and is started by the C main(). */

#include "target.h"

/* The macro that the float writers below are made with. */
#define FLOAT_WRITER                                                           \
    "/* Defines the function that prints one float type. Prints the fewest\n" \
    "   digits that read back as the same value; values are formatted through\n" \
    "   long double, the widest type printf accepts. */\n"                    \
    "#define WIND_WRITE_FLOAT(name, T, max_digits, parse) \\\n"              \
    "    static void name(T v) { \\\n"                                       \
    "        char buf[64]; \\\n"                                             \
    "        T shown = (T)(long double)v; \\\n"                              \
    "        for (int digits = 1; digits <= max_digits; digits++) { \\\n"    \
    "            snprintf(buf, sizeof buf, \"%.*Lg\", digits, (long double)v); \\\n" \
    "            if ((T)parse(buf, NULL) == shown) break; \\\n"              \
    "        } \\\n"                                                         \
    "        wind_put(buf, strlen(buf)); \\\n"                               \
    "    }\n"

static const HelperInfo host_helpers[HELPER_COUNT] = {
    [HELPER_PUT] = {
        "static void wind_put(const char *bytes, size_t count) {\n"
        "    if (count) fwrite(bytes, 1, count, stdout);\n"
        "}\n",
        HEADER_STDDEF | HEADER_STDIO, {0}, {0}},
    [HELPER_GET] = {
        "static int wind_get(void) {\n"
        "    int c = getchar();\n"
        "    return c == EOF ? -1 : c;\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_UNGET] = {
        "static void wind_unget(int c) {\n"
        "    ungetc(c, stdin);\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_FLUSH] = {
        "static void wind_flush(void) {\n"
        "    fflush(stdout);\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_ERROR] = {
        "static void wind_error(const char *text) {\n"
        "    fputs(text, stderr);\n"
        "}\n",
        HEADER_STDIO, {0}, {0}},
    [HELPER_STOP] = {
        "static _Noreturn void wind_stop(void) {\n"
        "    exit(1);\n"
        "}\n",
        HEADER_STDLIB, {0}, {0}},
    [HELPER_PARSE_FLOAT] = {
        "static long double wind_parse_float(const char *text, char **end) {\n"
        "    return strtold(text, end);\n"
        "}\n",
        HEADER_STDLIB, {0}, {0}},
    [HELPER_FLOAT_SUPPORT] = {FLOAT_WRITER, HEADER_STDIO | HEADER_STDLIB | HEADER_STRING,
                              {HELPER_PUT}, {0}},
    [HELPER_WRITE_F16] = {"WIND_WRITE_FLOAT(wind_write_f16, wind_f16, 5, strtof)\n", 0,
                          {HELPER_FLOAT_SUPPORT}, {TYPE_F16}},
    [HELPER_WRITE_F32] = {"WIND_WRITE_FLOAT(wind_write_f32, wind_f32, 9, strtof)\n", 0,
                          {HELPER_FLOAT_SUPPORT}, {TYPE_F32}},
    [HELPER_WRITE_F64] = {"WIND_WRITE_FLOAT(wind_write_f64, wind_f64, 17, strtod)\n", 0,
                          {HELPER_FLOAT_SUPPORT}, {TYPE_F64}},
    [HELPER_WRITE_F128] = {"WIND_WRITE_FLOAT(wind_write_f128, wind_f128, 21, strtold)\n", 0,
                           {HELPER_FLOAT_SUPPORT}, {TYPE_F128}},
};

const Target target_host = {
    .platform = {
        .name = "host",
        .pointer_size = sizeof(void *),
        .double_size = 8,
        .long_double_size = 16,
        .wide_integers = true,
        .has_arguments = true,
        .repeat_function = NULL,
    },
    .helpers = host_helpers,
    .start = "int main(void)",
    .start_with_arguments = "int main(int argc, char **argv)",
    .start_returns_status = true,
    .repeat = NULL,
};
