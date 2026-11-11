#ifndef WIND_TYPES_H
#define WIND_TYPES_H

#include "common.h"

typedef enum {
    /* The type of an expression that failed to check. It is compatible with
       everything, so one mistake is reported once. */
    TYPE_INVALID,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_INT,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_I128,
    TYPE_UINT,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_U128,
    TYPE_UINTPTR,
    TYPE_FLOAT,
    TYPE_F16,
    TYPE_F32,
    TYPE_F64,
    TYPE_F128,
} TypeKind;

#define TYPE_COUNT (TYPE_F128 + 1)

const char *type_name(TypeKind type);
/* The name of the C typedef the generated code uses for the type. */
const char *type_c_name(TypeKind type);

bool type_is_integer(TypeKind type);
bool type_is_signed(TypeKind type);
bool type_is_float(TypeKind type);
bool type_is_numeric(TypeKind type);

/* For integer types: the largest value, and the magnitude of the smallest
   (0 for unsigned types). */
U128 type_int_max(TypeKind type);
U128 type_int_min_magnitude(TypeKind type);

/* The number of bytes a value of the type occupies; 0 for void. */
size_t type_size(TypeKind type);

#endif
