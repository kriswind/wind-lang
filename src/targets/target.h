#ifndef WIND_TARGET_H
#define WIND_TARGET_H

#include "../types.h"

/*
 * A target is what a program is compiled for. The generated C is the same
 * for every target except in three places, which a Target describes:
 *
 * - the platform: the sizes of types and which ones exist, which checking
 *   the program depends on;
 * - the helpers that reach outside the program: where output goes, where
 *   input comes from, how errors are shown and how the program stops, and
 *   how floats are written and read;
 * - how the program starts: the C function its top level becomes, and the
 *   one that runs its repeat function.
 *
 * Every other helper is written in terms of the ones a target supplies, and
 * lives in codegen.c.
 */

/* The C headers that generated code relies on, as flags. */
typedef enum {
    HEADER_ERRNO = 1 << 0,
    HEADER_STDDEF = 1 << 1,
    HEADER_STDINT = 1 << 2,
    HEADER_STDIO = 1 << 3,
    HEADER_STDLIB = 1 << 4,
    HEADER_STRING = 1 << 5,
} Header;

/* The runtime support that generated code calls, listed so that each comes
   after the ones its definition calls. */
typedef enum {
    HELPER_NONE, /* ends a helper's list of needs */
    HELPER_HINTS,

    /* Supplied by the target. */
    HELPER_PUT,           /* void wind_put(const char *bytes, size_t count) */
    HELPER_GET,           /* int wind_get(void): the next input byte, or -1 */
    HELPER_UNGET,         /* void wind_unget(int c): gives a byte back */
    HELPER_FLUSH,         /* void wind_flush(void): before input is awaited */
    HELPER_ERROR,         /* void wind_error(const char *text) */
    HELPER_STOP,          /* void wind_stop(void): ends the program after an error */
    HELPER_PARSE_FLOAT,   /* long double wind_parse_float(const char *, char **) */
    HELPER_FLOAT_SUPPORT, /* whatever the float writers below share */
    HELPER_WRITE_F16,
    HELPER_WRITE_F32,
    HELPER_WRITE_F64,
    HELPER_WRITE_F128,

    /* The same on every target. */
    HELPER_DIGITS,
    HELPER_ERROR_NUMBER,
    HELPER_SHL,
    HELPER_SHR_UNSIGNED,
    HELPER_SHR_SIGNED,
    HELPER_SHL_128,
    HELPER_SHR_U128,
    HELPER_SHR_I128,
    HELPER_INDEX,
    HELPER_INDEX_UNCHECKED,
    HELPER_DEREF,
    HELPER_STRING_AT,
    HELPER_STRING_AT_UNCHECKED,
    HELPER_ARGS,
    HELPER_CIN_STATE,
    HELPER_IS_SPACE,
    HELPER_READ_WORD,
    HELPER_PARSE_U128,
    HELPER_READ_STRING,
    HELPER_READ_BOOL,
    HELPER_READ_CHAR,
    HELPER_READ_INT,
    HELPER_READ_UINT,
    HELPER_READ_U128,
    HELPER_READ_I128,
    HELPER_READ_FLOAT,
    HELPER_WRITE_STRING,
    HELPER_STRING_EQ,
    HELPER_WRITE_BOOL,
    HELPER_WRITE_CHAR,
    HELPER_WRITE_INT,
    HELPER_WRITE_UINT,
    HELPER_WRITE_U128,
    HELPER_WRITE_I128,
    HELPER_WRITE_POINTER,
    HELPER_WRITE_CSTRING,
    HELPER_STRING_FROM,
    HELPER_CSTRING,
    HELPER_WRITE_ESCAPED,
    HELPER_WRITE_QUOTED_CSTRING,
    HELPER_WRITE_QUOTED_STRING,
    HELPER_WRITE_QUOTED_CHAR,
    HELPER_COUNT,
} Helper;

/* The first and last helpers that a target supplies. */
#define HELPER_TARGET_FIRST HELPER_PUT
#define HELPER_TARGET_LAST HELPER_WRITE_F128

typedef struct {
    const char *definition;
    unsigned headers;  /* Header flags its definition relies on */
    Helper needs[4];   /* other helpers its definition calls */
    TypeKind types[3]; /* types its definition mentions */
} HelperInfo;

typedef struct Target {
    Platform platform;
    /* Indexed by helper; filled in for HELPER_TARGET_FIRST up to
       HELPER_TARGET_LAST. */
    const HelperInfo *helpers;
    /* The head of the C function that runs the program's top level, and
       then its main: without the program's arguments, and with them (NULL
       if the platform has none). */
    const char *start;
    const char *start_with_arguments;
    /* The C function returns the program's exit status. */
    bool start_returns_status;
    /* The head of the C function that calls the platform's repeat function
       once, or NULL. */
    const char *repeat;
    /* Text the generated C starts with, or NULL. */
    const char *prologue;
    /* What "wind help -t name" prints about the target. */
    const char *help;
    /* For a target whose programs run on a board: compiles the sketch in
       folder for the board named by board, uploads it through port, and
       then shows what the program prints until it is interrupted. A NULL
       port is the one the board is found on. Returns what "wind run" exits
       with, having reported any problem itself. NULL for a target whose
       programs run here. */
    int (*run)(const char *folder, const char *board, const char *port);
} Target;

extern const Target target_host;
extern const Target target_arduino;

/* The names of the targets, as messages list them. */
#define TARGET_NAMES "c and arduino"

/* The target of the given name, or NULL if there is none. */
const Target *target_find(Str name);

/* The Arduino sketch file that goes beside the generated C and connects it
   to the board. */
extern const char arduino_sketch[];

#endif
