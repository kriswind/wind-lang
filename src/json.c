#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds recursion so hostile input cannot overflow the stack. */
#define JSON_MAX_DEPTH 128

typedef struct {
    Arena *arena;
    const char *p;
    const char *end;
    int depth;
} JsonParser;

static Json *parse_value(JsonParser *jp);

static void skip_ws(JsonParser *jp) {
    while (jp->p < jp->end &&
           (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) {
        jp->p++;
    }
}

static bool eat(JsonParser *jp, char c) {
    if (jp->p < jp->end && *jp->p == c) {
        jp->p++;
        return true;
    }
    return false;
}

static bool eat_word(JsonParser *jp, const char *word) {
    size_t len = strlen(word);
    if ((size_t)(jp->end - jp->p) < len || memcmp(jp->p, word, len) != 0) {
        return false;
    }
    jp->p += len;
    return true;
}

static Json *new_json(JsonParser *jp, JsonKind kind) {
    Json *json = arena_alloc(jp->arena, sizeof *json);
    memset(json, 0, sizeof *json);
    json->kind = kind;
    return json;
}

static bool parse_hex4(JsonParser *jp, unsigned *out) {
    if (jp->end - jp->p < 4) return false;
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        char c = *jp->p++;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else return false;
        value = value * 16 + digit;
    }
    *out = value;
    return true;
}

static size_t encode_utf8(unsigned cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Called with the opening quote already consumed. */
static bool parse_string(JsonParser *jp, Str *out) {
    /* Every escape decodes to no more bytes than its spelling occupies. */
    char *buf = arena_alloc(jp->arena, (size_t)(jp->end - jp->p));
    size_t len = 0;
    for (;;) {
        if (jp->p >= jp->end) return false;
        char c = *jp->p++;
        if (c == '"') break;
        if (c != '\\') {
            buf[len++] = c;
            continue;
        }
        if (jp->p >= jp->end) return false;
        switch (*jp->p++) {
        case '"':  buf[len++] = '"';  break;
        case '\\': buf[len++] = '\\'; break;
        case '/':  buf[len++] = '/';  break;
        case 'b':  buf[len++] = '\b'; break;
        case 'f':  buf[len++] = '\f'; break;
        case 'n':  buf[len++] = '\n'; break;
        case 'r':  buf[len++] = '\r'; break;
        case 't':  buf[len++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (!parse_hex4(jp, &cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && jp->end - jp->p >= 6 &&
                jp->p[0] == '\\' && jp->p[1] == 'u') {
                const char *saved = jp->p;
                unsigned low;
                jp->p += 2;
                if (parse_hex4(jp, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    jp->p = saved;
                }
            }
            /* An unpaired surrogate has no UTF-8 encoding. */
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            len += encode_utf8(cp, buf + len);
            break;
        }
        default:
            return false;
        }
    }
    *out = (Str){buf, len};
    return true;
}

static Json *parse_number(JsonParser *jp) {
    char text[64];
    size_t len = 0;
    while (jp->p < jp->end && len < sizeof text - 1 &&
           (strchr("+-.eE", *jp->p) || (*jp->p >= '0' && *jp->p <= '9'))) {
        text[len++] = *jp->p++;
    }
    text[len] = '\0';
    char *end;
    double number = strtod(text, &end);
    if (len == 0 || *end != '\0') return NULL;
    Json *json = new_json(jp, JSON_NUMBER);
    json->as.number = number;
    return json;
}

static Json *parse_array(JsonParser *jp) {
    Json *json = new_json(jp, JSON_ARRAY);
    size_t cap = 0;
    skip_ws(jp);
    if (eat(jp, ']')) return json;
    for (;;) {
        Json *item = parse_value(jp);
        if (!item) return NULL;
        if (json->as.array.count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            json->as.array.items =
                arena_grow(jp->arena, json->as.array.items, cap * sizeof(Json *),
                           new_cap * sizeof(Json *));
            cap = new_cap;
        }
        json->as.array.items[json->as.array.count++] = item;
        skip_ws(jp);
        if (eat(jp, ']')) return json;
        if (!eat(jp, ',')) return NULL;
    }
}

static Json *parse_object(JsonParser *jp) {
    Json *json = new_json(jp, JSON_OBJECT);
    size_t cap = 0;
    skip_ws(jp);
    if (eat(jp, '}')) return json;
    for (;;) {
        Str key;
        skip_ws(jp);
        if (!eat(jp, '"') || !parse_string(jp, &key)) return NULL;
        skip_ws(jp);
        if (!eat(jp, ':')) return NULL;
        Json *value = parse_value(jp);
        if (!value) return NULL;
        if (json->as.object.count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            json->as.object.keys =
                arena_grow(jp->arena, json->as.object.keys, cap * sizeof(Str),
                           new_cap * sizeof(Str));
            json->as.object.values =
                arena_grow(jp->arena, json->as.object.values,
                           cap * sizeof(Json *), new_cap * sizeof(Json *));
            cap = new_cap;
        }
        json->as.object.keys[json->as.object.count] = key;
        json->as.object.values[json->as.object.count] = value;
        json->as.object.count++;
        skip_ws(jp);
        if (eat(jp, '}')) return json;
        if (!eat(jp, ',')) return NULL;
    }
}

static Json *parse_value(JsonParser *jp) {
    skip_ws(jp);
    if (jp->p >= jp->end) return NULL;
    if (eat_word(jp, "null")) return new_json(jp, JSON_NULL);
    if (eat_word(jp, "true")) {
        Json *json = new_json(jp, JSON_BOOL);
        json->as.boolean = true;
        return json;
    }
    if (eat_word(jp, "false")) return new_json(jp, JSON_BOOL);
    if (eat(jp, '"')) {
        Json *json = new_json(jp, JSON_STRING);
        return parse_string(jp, &json->as.string) ? json : NULL;
    }
    if (*jp->p == '[' || *jp->p == '{') {
        if (jp->depth >= JSON_MAX_DEPTH) return NULL;
        jp->depth++;
        Json *json = (*jp->p++ == '[') ? parse_array(jp) : parse_object(jp);
        jp->depth--;
        return json;
    }
    return parse_number(jp);
}

Json *json_parse(Arena *arena, Str text) {
    JsonParser jp = {arena, text.data, text.data + text.len, 0};
    Json *json = parse_value(&jp);
    if (!json) return NULL;
    skip_ws(&jp);
    return jp.p == jp.end ? json : NULL;
}

const Json *json_get(const Json *object, const char *key) {
    if (!object || object->kind != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < object->as.object.count; i++) {
        if (str_eq_cstr(object->as.object.keys[i], key)) {
            return object->as.object.values[i];
        }
    }
    return NULL;
}

size_t json_count(const Json *array) {
    return (array && array->kind == JSON_ARRAY) ? array->as.array.count : 0;
}

const Json *json_at(const Json *array, size_t index) {
    return index < json_count(array) ? array->as.array.items[index] : NULL;
}

bool json_string(const Json *value, Str *out) {
    if (!value || value->kind != JSON_STRING) return false;
    *out = value->as.string;
    return true;
}

bool json_integer(const Json *value, long *out) {
    if (!value || value->kind != JSON_NUMBER) return false;
    double number = value->as.number;
    if (number < -1e15 || number > 1e15) return false;
    *out = (long)number;
    return true;
}

void buf_append(Buf *buf, const char *data, size_t len) {
    if (buf->cap - buf->len < len) {
        size_t cap = buf->cap ? buf->cap * 2 : 256;
        while (cap - buf->len < len) cap *= 2;
        buf->data = arena_grow(buf->arena, buf->data, buf->len, cap);
        buf->cap = cap;
    }
    if (len) memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void buf_puts(Buf *buf, const char *text) {
    buf_append(buf, text, strlen(text));
}

void buf_printf(Buf *buf, const char *fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    if (len < 0) return;
    if ((size_t)len < sizeof text) {
        buf_append(buf, text, (size_t)len);
        return;
    }
    char *big = arena_alloc(buf->arena, (size_t)len + 1);
    va_start(args, fmt);
    vsnprintf(big, (size_t)len + 1, fmt, args);
    va_end(args);
    buf_append(buf, big, (size_t)len);
}

void buf_json_string(Buf *buf, Str s) {
    buf_append(buf, "\"", 1);
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
        case '"':  buf_puts(buf, "\\\""); break;
        case '\\': buf_puts(buf, "\\\\"); break;
        case '\n': buf_puts(buf, "\\n");  break;
        case '\r': buf_puts(buf, "\\r");  break;
        case '\t': buf_puts(buf, "\\t");  break;
        default:
            if (c < 0x20) {
                buf_printf(buf, "\\u%04x", c);
            } else {
                buf_append(buf, (const char *)&c, 1);
            }
        }
    }
    buf_append(buf, "\"", 1);
}

void buf_json_scalar(Buf *buf, const Json *value) {
    long integer;
    if (!value) {
        buf_puts(buf, "null");
    } else if (value->kind == JSON_STRING) {
        buf_json_string(buf, value->as.string);
    } else if (value->kind == JSON_BOOL) {
        buf_puts(buf, value->as.boolean ? "true" : "false");
    } else if (value->kind == JSON_NUMBER && json_integer(value, &integer) &&
               (double)integer == value->as.number) {
        buf_printf(buf, "%ld", integer);
    } else if (value->kind == JSON_NUMBER) {
        buf_printf(buf, "%.17g", value->as.number);
    } else {
        buf_puts(buf, "null");
    }
}
