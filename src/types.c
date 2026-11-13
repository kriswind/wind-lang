#include "types.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    CLASS_OTHER,
    CLASS_SIGNED,
    CLASS_UNSIGNED,
    CLASS_FLOAT,
} TypeClass;

/* bits is 0 for the pointer-sized integers, whose width depends on the
   target. */
static const struct {
    const char *name;
    const char *c_name;
    TypeClass class;
    int bits;
} types[] = {
    [TYPE_INVALID] = {"<invalid>", "void", CLASS_OTHER, 0},
    [TYPE_VOID]    = {"void", "void", CLASS_OTHER, 0},
    [TYPE_BOOL]    = {"bool", "wind_bool", CLASS_OTHER, 0},
    [TYPE_STRING]  = {"string", "wind_string", CLASS_OTHER, 0},
    [TYPE_CHAR]    = {"char", "wind_char", CLASS_OTHER, 0},
    [TYPE_INT]     = {"int", "wind_int", CLASS_SIGNED, 0},
    [TYPE_I8]      = {"i8", "wind_i8", CLASS_SIGNED, 8},
    [TYPE_I16]     = {"i16", "wind_i16", CLASS_SIGNED, 16},
    [TYPE_I32]     = {"i32", "wind_i32", CLASS_SIGNED, 32},
    [TYPE_I64]     = {"i64", "wind_i64", CLASS_SIGNED, 64},
    [TYPE_I128]    = {"i128", "wind_i128", CLASS_SIGNED, 128},
    [TYPE_UINT]    = {"uint", "wind_uint", CLASS_UNSIGNED, 0},
    [TYPE_U8]      = {"u8", "wind_u8", CLASS_UNSIGNED, 8},
    [TYPE_U16]     = {"u16", "wind_u16", CLASS_UNSIGNED, 16},
    [TYPE_U32]     = {"u32", "wind_u32", CLASS_UNSIGNED, 32},
    [TYPE_U64]     = {"u64", "wind_u64", CLASS_UNSIGNED, 64},
    [TYPE_U128]    = {"u128", "wind_u128", CLASS_UNSIGNED, 128},
    [TYPE_UINTPTR] = {"uintptr", "wind_uintptr", CLASS_UNSIGNED, 0},
    [TYPE_FLOAT]   = {"float", "wind_float", CLASS_FLOAT, 64},
    [TYPE_F16]     = {"f16", "wind_f16", CLASS_FLOAT, 16},
    [TYPE_F32]     = {"f32", "wind_f32", CLASS_FLOAT, 32},
    [TYPE_F64]     = {"f64", "wind_f64", CLASS_FLOAT, 64},
    [TYPE_F128]    = {"f128", "wind_f128", CLASS_FLOAT, 128},
};

static TypeTable *table;

void types_use(TypeTable *in_use) {
    table = in_use;
}

size_t types_count(void) {
    return table ? table->count : 0;
}

TypeInfo *type_info(TypeKind type) {
    size_t index = (size_t)type - TYPE_COUNT;
    if (type < TYPE_COUNT || !table || index >= table->count) return NULL;
    return table->items[index];
}

static char *format_name(const char *fmt, int number, Str name) {
    int len = snprintf(NULL, 0, fmt, number, (int)name.len, name.data);
    char *text = arena_alloc(table->arena, (size_t)len + 1);
    snprintf(text, (size_t)len + 1, fmt, number, (int)name.len, name.data);
    return text;
}

TypeKind type_add(bool is_enum, Str name, Str qualifier) {
    TypeInfo *info = arena_alloc(table->arena, sizeof *info);
    memset(info, 0, sizeof *info);
    int number = (int)table->count;
    info->is_enum = is_enum;
    char *short_name = arena_alloc(table->arena, name.len + 1);
    memcpy(short_name, name.data, name.len);
    short_name[name.len] = '\0';
    info->short_name = short_name;
    info->name = short_name;
    if (qualifier.len) {
        char *full = arena_alloc(table->arena, qualifier.len + name.len + 2);
        snprintf(full, qualifier.len + name.len + 2, "%.*s.%s", (int)qualifier.len,
                 qualifier.data, short_name);
        info->name = full;
    }
    /* The number keeps apart types of one name declared in different files. */
    info->c_name = format_name("wind_t%d_%.*s", number, name);
    info->writer_name = format_name("wind_write_t%d_%.*s", number, name);
    info->c_stem = format_name("t%d_%.*s", number, name);
    /* An enum is stored as C stores one: in an int. */
    info->size = is_enum ? 4 : 0;
    info->align = is_enum ? 4 : 1;
    ARENA_PUSH(table->arena, table->items, table->count, table->cap, info);
    return (TypeKind)(TYPE_COUNT + number);
}

void type_layout(TypeInfo *info) {
    size_t size = 0, align = 1;
    for (size_t i = 0; i < info->field_count; i++) {
        size_t field_align = type_align(info->fields[i].type);
        if (field_align > align) align = field_align;
        size = (size + field_align - 1) / field_align * field_align;
        size += type_size(info->fields[i].type);
    }
    info->size = (size + align - 1) / align * align;
    info->align = align;
}

bool type_is_struct(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && !info->is_enum;
}

bool type_is_enum(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->is_enum;
}

int type_field_index(const TypeInfo *info, Str name) {
    for (size_t i = 0; i < info->field_count; i++) {
        if (str_eq(info->fields[i].name, name)) return (int)i;
    }
    return -1;
}

static TypeClass class_of(TypeKind type) {
    return type < TYPE_COUNT ? types[type].class : CLASS_OTHER;
}

const char *type_name(TypeKind type) {
    const TypeInfo *info = type_info(type);
    if (info) return info->name;
    return type < TYPE_COUNT ? types[type].name : "<invalid>";
}

const char *type_c_name(TypeKind type) {
    const TypeInfo *info = type_info(type);
    if (info) return info->c_name;
    return type < TYPE_COUNT ? types[type].c_name : "void";
}

bool type_is_signed(TypeKind type) {
    return class_of(type) == CLASS_SIGNED;
}

bool type_is_integer(TypeKind type) {
    return class_of(type) == CLASS_SIGNED || class_of(type) == CLASS_UNSIGNED;
}

bool type_is_float(TypeKind type) {
    return class_of(type) == CLASS_FLOAT;
}

bool type_is_numeric(TypeKind type) {
    return type_is_integer(type) || type_is_float(type);
}

/* Programs are compiled for the machine the compiler runs on, so its pointer
   size is the target's. */
static int int_bits(TypeKind type) {
    return types[type].bits ? types[type].bits : (int)(sizeof(void *) * 8);
}

size_t type_size(TypeKind type) {
    const TypeInfo *info = type_info(type);
    if (info) return info->size;
    if (type >= TYPE_COUNT) return 0;
    switch (type) {
    case TYPE_INVALID:
    case TYPE_VOID:
        return 0;
    case TYPE_BOOL:
    case TYPE_CHAR:
        return 1;
    case TYPE_STRING:
        return 2 * sizeof(void *); /* a pointer and a length */
    default:
        return (size_t)int_bits(type) / 8;
    }
}

size_t type_align(TypeKind type) {
    const TypeInfo *info = type_info(type);
    if (info) return info->align;
    /* A string is aligned as the pointer it starts with; every other
       built-in type is aligned to its own size. */
    if (type == TYPE_STRING) return sizeof(void *);
    size_t size = type_size(type);
    return size ? size : 1;
}

U128 type_int_max(TypeKind type) {
    int bits = int_bits(type) - (type_is_signed(type) ? 1 : 0);
    return bits == 128 ? ~(U128)0 : ((U128)1 << bits) - 1;
}

U128 type_int_min_magnitude(TypeKind type) {
    return type_is_signed(type) ? (U128)1 << (int_bits(type) - 1) : 0;
}
