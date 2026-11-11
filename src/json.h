#ifndef WIND_JSON_H
#define WIND_JSON_H

#include "common.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct Json Json;
struct Json {
    JsonKind kind;
    union {
        bool boolean;
        double number;
        Str string; /* decoded UTF-8 */
        struct {
            Json **items;
            size_t count;
        } array;
        struct {
            Str *keys;
            Json **values;
            size_t count;
        } object;
    } as;
};

/* Returns NULL if text is not exactly one well-formed JSON value. */
Json *json_parse(Arena *arena, Str text);

/* Accessors tolerate NULL and values of the wrong kind, so lookups can be
   chained without checking each step. */
const Json *json_get(const Json *object, const char *key);
const Json *json_at(const Json *array, size_t index);
size_t json_count(const Json *array);
bool json_string(const Json *value, Str *out);
bool json_integer(const Json *value, long *out);

/* A growable byte buffer backed by an arena. */
typedef struct {
    Arena *arena;
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_append(Buf *buf, const char *data, size_t len);
void buf_puts(Buf *buf, const char *text);
void buf_printf(Buf *buf, const char *fmt, ...);
/* Appends s as a quoted JSON string. s must be valid UTF-8. */
void buf_json_string(Buf *buf, Str s);
/* Appends a scalar JSON value; arrays and objects are written as null. */
void buf_json_scalar(Buf *buf, const Json *value);

#endif
