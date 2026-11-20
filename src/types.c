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

const Platform platform_host = {
    .name = "c",
    .pointer_size = sizeof(void *),
    .double_size = 8,
    .long_double_size = 16,
    .wide_integers = true,
    .has_arguments = true,
    .repeat_function = NULL,
};

static const Platform *platform = &platform_host;

void types_use_platform(const Platform *in_use) {
    platform = in_use;
}

const Platform *types_platform(void) {
    return platform;
}

bool type_available(TypeKind type) {
    return platform->wide_integers || (type != TYPE_I128 && type != TYPE_U128);
}

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

static char *format_text(const char *fmt, const char *text) {
    int len = snprintf(NULL, 0, fmt, text);
    char *out = arena_alloc(table->arena, (size_t)len + 1);
    snprintf(out, (size_t)len + 1, fmt, text);
    return out;
}

TypeKind type_pointer_to(TypeKind target) {
    for (size_t i = 0; i < table->count; i++) {
        const TypeInfo *known = table->items[i];
        if (known->is_pointer && known->target == target) return (TypeKind)(TYPE_COUNT + i);
    }
    const TypeInfo *pointee = type_info(target);
    TypeInfo *info = arena_alloc(table->arena, sizeof *info);
    memset(info, 0, sizeof *info);
    info->is_pointer = true;
    info->target = target;
    /* "int *", and "int **" for a pointer to that. */
    info->name = format_text(pointee && pointee->is_pointer ? "%s*" : "%s *",
                             type_name(target));
    info->short_name = info->name;
    info->c_stem = format_text("p_%s", pointee ? pointee->c_stem : type_name(target));
    info->c_name = format_text("wind_%s", info->c_stem);
    info->writer_name = "wind_write_pointer";
    info->size = platform->pointer_size;
    info->align = platform->pointer_size;
    ARENA_PUSH(table->arena, table->items, table->count, table->cap, info);
    return (TypeKind)(TYPE_COUNT + table->count - 1);
}

/* The stem of names derived from a type in the generated C. */
static const char *stem_of(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info ? info->c_stem : type_name(type);
}

static char *format_two(const char *fmt, const char *a, const char *b) {
    int len = snprintf(NULL, 0, fmt, a, b);
    char *out = arena_alloc(table->arena, (size_t)len + 1);
    snprintf(out, (size_t)len + 1, fmt, a, b);
    return out;
}

/* Finds or makes a container of elem; key and length as its kind has
   them, and zero otherwise. */
static TypeKind container_of(ContainerKind kind, TypeKind elem, TypeKind key, size_t length) {
    for (size_t i = 0; i < table->count; i++) {
        const TypeInfo *known = table->items[i];
        if (known->container == kind && known->target == elem && known->key == key &&
            known->length == length) {
            return (TypeKind)(TYPE_COUNT + i);
        }
    }
    TypeInfo *info = arena_alloc(table->arena, sizeof *info);
    memset(info, 0, sizeof *info);
    info->container = kind;
    info->target = elem;
    info->key = key;
    info->length = length;
    /* "int[3][4]" is an array of 3 "int[4]": this container's part of the
       name comes before its elements' part. */
    const TypeInfo *inner = type_info(elem);
    bool nested = inner && inner->container;
    info->base = nested ? inner->base : elem;
    char own[64];
    switch (kind) {
    case CONTAINER_ARRAY:
        snprintf(own, sizeof own, "[%zu]", length);
        break;
    case CONTAINER_SLICE:
        snprintf(own, sizeof own, "[]");
        break;
    case CONTAINER_LIST:
        snprintf(own, sizeof own, "[..]");
        break;
    default:
        snprintf(own, sizeof own, "[%s]", type_name(key));
        break;
    }
    info->suffix = format_two("%s%s", own, nested ? inner->suffix : "");
    info->name = format_two("%s%s", type_name(info->base), info->suffix);
    info->short_name = info->name;
    const char *elem_stem = stem_of(elem);
    char count[32];
    snprintf(count, sizeof count, "%zu", length);
    switch (kind) {
    case CONTAINER_ARRAY:
        info->c_stem = format_two("array%s_%s", count, elem_stem);
        info->c_name = format_text("wind_%s", info->c_stem);
        info->size = type_size(elem) * length;
        info->align = type_align(elem);
        break;
    case CONTAINER_SLICE:
        info->c_stem = format_text("slice_%s", elem_stem);
        info->c_name = format_text("wind_slice_%s", elem_stem);
        info->size = 2 * platform->pointer_size;
        info->align = platform->pointer_size;
        break;
    case CONTAINER_LIST:
        info->c_stem = format_text("list_%s", elem_stem);
        info->c_name = format_text("wind_list_%s", elem_stem);
        info->size = 3 * platform->pointer_size;
        info->align = platform->pointer_size;
        break;
    default:
        /* Three pointers and five counts, whatever it holds. */
        info->c_stem = format_two("map_%s_%s", stem_of(key), elem_stem);
        info->c_name = "wind_map";
        info->size = 8 * platform->pointer_size;
        info->align = platform->pointer_size;
        break;
    }
    info->writer_name = format_text("wind_write_%s", info->c_stem);
    ARENA_PUSH(table->arena, table->items, table->count, table->cap, info);
    return (TypeKind)(TYPE_COUNT + table->count - 1);
}

TypeKind type_array_of(TypeKind elem, size_t length) {
    return container_of(CONTAINER_ARRAY, elem, TYPE_INVALID, length);
}

TypeKind type_slice_of(TypeKind elem) {
    return container_of(CONTAINER_SLICE, elem, TYPE_INVALID, 0);
}

TypeKind type_list_of(TypeKind elem) {
    return container_of(CONTAINER_LIST, elem, TYPE_INVALID, 0);
}

TypeKind type_map_of(TypeKind key, TypeKind value) {
    return container_of(CONTAINER_MAP, value, key, 0);
}

ContainerKind type_container(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info ? info->container : CONTAINER_NONE;
}

bool type_is_container(TypeKind type) {
    return type_container(type) != CONTAINER_NONE;
}

TypeKind type_elem(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->container ? info->target : TYPE_INVALID;
}

TypeKind type_key(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->container == CONTAINER_MAP ? info->key : TYPE_INVALID;
}

size_t type_length(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->container == CONTAINER_ARRAY ? info->length : 0;
}

TypeKind type_base(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->container ? info->base : type;
}

const char *type_suffix(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->container ? info->suffix : "";
}

void type_layout(TypeInfo *info) {
    size_t size = 0, align = 1;
    for (size_t i = 0; i < info->field_count; i++) {
        const Field *field = &info->fields[i];
        size_t field_align = type_align(field->type);
        size_t field_size = type_size(field->type);
        if (field_align > align) align = field_align;
        size = (size + field_align - 1) / field_align * field_align;
        size += field_size;
    }
    info->size = (size + align - 1) / align * align;
    info->align = align;
}

bool type_is_struct(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && !info->is_enum && !info->is_pointer && !info->container;
}

bool type_is_enum(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->is_enum;
}

bool type_is_pointer(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->is_pointer;
}

TypeKind type_target(TypeKind type) {
    const TypeInfo *info = type_info(type);
    return info && info->is_pointer ? info->target : TYPE_INVALID;
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

/* The pointer-sized integers take their width from the platform. */
static int int_bits(TypeKind type) {
    return types[type].bits ? types[type].bits : (int)(platform->pointer_size * 8);
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
        return 2 * platform->pointer_size; /* a pointer and a length */
    case TYPE_FLOAT:
    case TYPE_F64:
        return platform->double_size;
    case TYPE_F128:
        return platform->long_double_size;
    default:
        return (size_t)int_bits(type) / 8;
    }
}

size_t type_align(TypeKind type) {
    const TypeInfo *info = type_info(type);
    if (info) return info->align;
    /* A string is aligned as the pointer it starts with; every other
       built-in type is aligned to its own size. */
    if (type == TYPE_STRING) return platform->pointer_size;
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
