#ifndef WIND_TYPES_H
#define WIND_TYPES_H

#include "common.h"

/* A type is identified by a number. The built-in types are the values named
   here; a struct or enum that a program declares gets the next free number
   from TYPE_COUNT up in its TypeTable, and is described by a TypeInfo. */
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

typedef struct {
    Str name;
    SrcLoc loc;
    TypeKind type;
} Field;

typedef struct {
    Str name;
    long long value;
} EnumValue;

typedef struct {
    bool is_enum; /* else a struct */
    /* The name as declared, and as diagnostics show it: for a type from an
       imported file, with the file's name in front, since two files may
       each declare a type of one name. */
    const char *short_name;
    const char *name;
    /* Names for the generated C: the type itself, the function that prints
       a value of it, and the stem shared by what is derived from the type. */
    const char *c_name;
    const char *writer_name;
    const char *c_stem;
    Field *fields; /* struct */
    size_t field_count;
    EnumValue *values; /* enum */
    size_t value_count;
    size_t size;
    size_t align;
    /* What the generated program needs for the type; maintained by emit_c. */
    bool writer_used;
    bool writer_emitted;
    bool slice_used;
    bool array_writer_used;
} TypeInfo;

/* The types one program declares. A zeroed table with its arena set is
   empty and ready to use. */
typedef struct {
    Arena *arena;
    TypeInfo **items;
    size_t count;
    size_t cap;
} TypeTable;

/* Selects the table that the functions below consult for declared types.
   It must be the table of the program being analyzed, emitted or described,
   and outlive that work. */
void types_use(TypeTable *table);
/* Adds a type to the table in use and returns its number. qualifier is the
   name of the imported file that declares it, or empty. The caller fills in
   the fields or values, and then calls type_layout for a struct. */
TypeKind type_add(bool is_enum, Str name, Str qualifier);
/* Computes a struct's size and alignment from its fields, as C lays it out. */
void type_layout(TypeInfo *info);
/* How many types the table in use holds; their numbers run from TYPE_COUNT. */
size_t types_count(void);
/* The description of a declared type; NULL for a built-in type. */
TypeInfo *type_info(TypeKind type);
bool type_is_struct(TypeKind type);
bool type_is_enum(TypeKind type);
/* The position of the named field in a struct, or -1. */
int type_field_index(const TypeInfo *info, Str name);

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
size_t type_align(TypeKind type);

#endif
