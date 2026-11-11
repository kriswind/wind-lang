#include "types.h"

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

const char *type_name(TypeKind type) {
    return types[type].name;
}

const char *type_c_name(TypeKind type) {
    return types[type].c_name;
}

bool type_is_signed(TypeKind type) {
    return types[type].class == CLASS_SIGNED;
}

bool type_is_integer(TypeKind type) {
    return types[type].class == CLASS_SIGNED || types[type].class == CLASS_UNSIGNED;
}

bool type_is_float(TypeKind type) {
    return types[type].class == CLASS_FLOAT;
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

U128 type_int_max(TypeKind type) {
    int bits = int_bits(type) - (type_is_signed(type) ? 1 : 0);
    return bits == 128 ? ~(U128)0 : ((U128)1 << bits) - 1;
}

U128 type_int_min_magnitude(TypeKind type) {
    return type_is_signed(type) ? (U128)1 << (int_bits(type) - 1) : 0;
}
