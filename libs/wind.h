/* Wind's types as C sees them, for an editor reading a header that a Wind
   library carries. In a program, Wind defines these itself, with
   WIND_PROGRAM, and this file is not read. Keep it as codegen writes them. */

#ifndef WIND_TYPES_FOR_EDITORS
#define WIND_TYPES_FOR_EDITORS

#include <stddef.h>
#include <stdint.h>

typedef _Bool wind_bool;
typedef unsigned char wind_char;
typedef intptr_t wind_int;
typedef int64_t wind_i64;
typedef double wind_f64;

typedef struct {
    const char *data;
    size_t len;
} wind_string;

typedef struct {
    wind_string *data;
    size_t len;
} wind_slice_string;

typedef struct {
    wind_char *data;
    size_t len;
    size_t cap;
} wind_list_char;

typedef struct {
    wind_string *data;
    size_t len;
    size_t cap;
} wind_list_string;

#endif
