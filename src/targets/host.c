/* The target for the machine the compiler runs on: a program reads stdin,
   writes stdout and stderr, and is started by the C main(). */

#include "target.h"

/* The macro that the float writers below are made with. */
#define FLOAT_WRITER                                                           \
    "/* Defines the function that prints one float type. Prints the fewest\n" \
    "   digits that read back as the same value; values are formatted through\n" \
    "   long double, the widest type printf accepts. A number that so few\n"  \
    "   digits would put in exponent form, as 2e+01 for 20, is written out\n"  \
    "   in full while that takes no more digits than the type holds. */\n"   \
    "#define WIND_WRITE_FLOAT(name, T, max_digits, parse) \\\n"              \
    "    static void name(T v) { \\\n"                                       \
    "        char buf[64]; \\\n"                                             \
    "        T shown = (T)(long double)v; \\\n"                              \
    "        for (int digits = 1; digits <= max_digits; digits++) { \\\n"    \
    "            snprintf(buf, sizeof buf, \"%.*Lg\", digits, (long double)v); \\\n" \
    "            if ((T)parse(buf, NULL) == shown) break; \\\n"              \
    "        } \\\n"                                                         \
    "        char *e = strchr(buf, 'e'); \\\n"                               \
    "        int exponent = e ? atoi(e + 1) : -1; \\\n"                      \
    "        if (exponent >= 0 && exponent < max_digits) { \\\n"             \
    "            snprintf(buf, sizeof buf, \"%.*Lg\", exponent + 1, (long double)v); \\\n" \
    "        } \\\n"                                                         \
    "        wind_put(buf, strlen(buf)); \\\n"                               \
    "    }\n"

static const HelperInfo host_helpers[HELPER_COUNT] = {
    [HELPER_PUT] = {
        "static void wind_put(const char *bytes, size_t count) {\n"
        "#ifdef WIND_SINK\n"
        "    if (wind_sink) {\n"
        "        wind_sink_put(bytes, count);\n"
        "        return;\n"
        "    }\n"
        "#endif\n"
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
    [HELPER_WRITE_FIXED] = {
        "/* Writes v with digits digits after the point, rounded. */\n"
        "static void wind_write_fixed(long double v, int digits) {\n"
        "    if (digits < 0) digits = 0;\n"
        "    if (digits > 40) digits = 40;\n"
        "    char small[64];\n"
        "    int len = snprintf(small, sizeof small, \"%.*Lf\", digits, v);\n"
        "    if (len < 0) return;\n"
        "    if ((size_t)len < sizeof small) {\n"
        "        wind_put(small, (size_t)len);\n"
        "        return;\n"
        "    }\n"
        "    char *text = malloc((size_t)len + 1);\n"
        "    if (!text) return;\n"
        "    snprintf(text, (size_t)len + 1, \"%.*Lf\", digits, v);\n"
        "    wind_put(text, (size_t)len);\n"
        "    free(text);\n"
        "}\n",
        HEADER_STDIO | HEADER_STDLIB, {HELPER_PUT}, {0}},
    [HELPER_EXIT] = {
        "static _Noreturn void wind_exit(int status) {\n"
        "    fflush(stdout);\n"
        "    exit(status);\n"
        "}\n",
        HEADER_STDIO | HEADER_STDLIB, {0}, {0}},
};

const Target target_host = {
    .platform = {
        .name = "c",
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
    .help =
        "Target c: a program for the machine wind runs on. This is the target\n"
        "when neither -t nor the file's #target names another.\n"
        "\n"
        "  wind build [-t c] <file.wind> [-o <output>]   compile to an executable\n"
        "  wind run [-t c] <file.wind> [args...]          compile and run\n"
        "  wind check [-t c] <file.wind>...               report errors and warnings\n"
        "  wind emit [-t c] <file.wind>                   print the generated C\n"
        "\n"
        "The program reads stdin and writes stdout. An error such as an index out\n"
        "of range is reported on stderr and ends the program with status 1. main\n"
        "may take the program's arguments as '(int argc, string argv[])', and the\n"
        "status an int main returns is the program's exit status.\n"
        "\n"
        "Sizes are this machine's: int, uint, uintptr and pointers are as wide as\n"
        "its pointers, and every type, i128 and u128 included, is available.\n"
        "\n"
        "The C compiler is taken from $CC (default: cc).\n",
};
