#ifndef WIND_AST_H
#define WIND_AST_H

#include "common.h"
#include "types.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Symbol Symbol;
typedef struct Unit Unit;

/* ---- Symbols ---- */

typedef enum {
    SYM_CONST,
    SYM_VAR,
    SYM_PARAM,
    SYM_FUNC,
    SYM_LABEL,
    SYM_MODULE,
    SYM_TYPE,       /* a struct, an enum, or another name for a type */
    SYM_ENUM_VALUE, /* one of an enum's names */
} SymbolKind;

/* A named thing a program declares. Created by unit_analyze. */
struct Symbol {
    SymbolKind kind;
    Str name;
    /* The name at its declaration. A module imported without "as" has no
       name of its own in the source, and this is its path. */
    SrcLoc loc;
    /* SYM_FUNC: the return type; SYM_TYPE: the type it names;
       SYM_ENUM_VALUE: its enum; SYM_LABEL, SYM_MODULE: unused; an array: the
       type of its elements. */
    TypeKind type;
    long long value; /* SYM_ENUM_VALUE */
    /* An array is either fixed (declared with a length, which it owns) or a
       slice (an array parameter: a view of someone else's elements, with a
       length known only at run time). */
    bool is_array;
    bool is_slice;
    size_t length; /* fixed arrays only */
    /* Unique within the unit; keeps generated C names from colliding. */
    int id;
    /* The file the symbol is declared in. */
    const Unit *unit;
    /* Marked "export": other files that import this one can use it. */
    bool exported;
    /* SYM_MODULE: the imported file, or NULL if it could not be loaded. */
    const Unit *module;
    /* The declaring statement: STMT_CONST, STMT_VAR, STMT_FUNC,
       STMT_IMPORT or STMT_TYPE. NULL for parameters and labels. */
    const Stmt *decl;
    /* The function the symbol is declared in; NULL at the top level. */
    const Stmt *func;
    /* Declared directly at the top level, outside any block, and so visible
       to the functions that follow it. */
    bool global;
    /* The source offsets between which the name can be referred to. */
    size_t visible_from;
    size_t visible_to;
    bool used;
};

typedef struct {
    Symbol **items;
    size_t count;
    size_t cap;
} SymbolList;

/* A place in the source that refers to a symbol. */
typedef struct {
    SrcLoc loc;
    const Symbol *symbol;
} Ref;

/* ---- Types as written ---- */

/* A type that is not spelled with a built-in name, and so is only known
   once the program is analyzed: a declared type's name, or typeof(expr). */
typedef struct {
    SrcLoc loc;
    Str name; /* empty for typeof */
    Str module; /* set for "module.Name" */
    SrcLoc module_loc;
    SrcLoc name_loc;
    Expr *of; /* typeof's operand */
} TypeRef;

/* ---- Expressions ---- */

typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_CHAR_LIT,
    EXPR_STRING_LIT,
    EXPR_BOOL_LIT,
    EXPR_IDENT,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_CAST,
    EXPR_TERNARY,
    /* "base.name": a field of a struct value. */
    EXPR_FIELD,
    /* "{ a, b }" or "{ .x = a, .y = b }": a struct value, whose type is the
       one its context asks for. */
    EXPR_INIT,
    /* "cin" used as a value: whether the most recent read succeeded. */
    EXPR_CIN_OK,
    EXPR_INDEX,
    /* The built-in len(x); a call to "len" becomes this during analysis. */
    EXPR_LEN,
    /* Replaced by an EXPR_INT_LIT holding the size during analysis. */
    EXPR_SIZEOF,
} ExprKind;

typedef enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
    OP_NEG,
    OP_NOT,
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_SHL,
    OP_SHR,
    OP_BIT_NOT,
} Op;

struct Expr {
    ExprKind kind;
    /* The whole expression when it fits on one line, else its first token. */
    SrcLoc loc;
    TypeKind type; /* set by unit_analyze */
    /* Written as "( expr )", with nothing outside the parentheses. */
    bool parenthesized;
    union {
        U128 int_lit;
        Str float_lit; /* spelling, without digit separators */
        char char_lit;
        Str string_lit; /* decoded bytes */
        bool bool_lit;
        /* "name", or "module.name" for something another file exports. */
        struct {
            Str name;
            SrcLoc name_loc;
            Str module; /* empty when the name is not qualified */
            SrcLoc module_loc;
            const Symbol *symbol; /* set by unit_analyze; NULL if undefined */
        } ident;
        struct {
            Op op;
            Expr *operand;
        } unary;
        struct {
            Op op;
            Expr *left;
            Expr *right;
        } binary;
        struct {
            Str name;
            SrcLoc name_loc;
            Str module; /* as for ident */
            SrcLoc module_loc;
            const Symbol *symbol; /* set by unit_analyze; NULL if undefined */
            Expr **args;
            size_t count;
        } call;
        struct {
            TypeKind to;
            TypeRef *to_ref; /* NULL for a built-in type; resolved into "to" */
            Expr *operand;
        } cast;
        struct {
            Expr *base;
            Str name;
            SrcLoc name_loc;
            int index; /* the field's position; set by unit_analyze */
        } field;
        struct {
            struct InitItem *items;
            size_t count;
        } init;
        /* "cond ? then : otherwise" */
        struct {
            Expr *cond;
            Expr *then;
            Expr *otherwise;
        } ternary;
        struct {
            TypeKind type; /* used when operand is NULL */
            TypeRef *type_ref; /* as for cast.to_ref */
            Expr *operand;
        } size_of;
        struct {
            /* An identifier naming an array, or any string expression. */
            Expr *base;
            Expr *index;
        } index;
        struct {
            Expr *operand; /* as for index.base */
        } len;
    } as;
};

/* One value of an EXPR_INIT. */
typedef struct InitItem {
    Str field; /* empty when the value is given by position */
    SrcLoc field_loc;
    Expr *value;
    int index; /* the field it sets; set by unit_analyze */
} InitItem;

/* ---- Statements ---- */

typedef enum {
    STMT_CONST,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_COUT,
    STMT_CIN,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_GOTO,
    STMT_LABEL,
    STMT_RETURN,
    STMT_FUNC,
    STMT_IMPORT,
    STMT_TYPE,
} StmtKind;

typedef enum {
    TYPE_DECL_STRUCT,
    TYPE_DECL_ENUM,
    TYPE_DECL_ALIAS, /* typedef */
} TypeDeclKind;

/* A field in a struct declaration, or a name in an enum declaration. */
typedef struct {
    Str name;
    SrcLoc loc;
    TypeKind type; /* field */
    TypeRef *type_ref;
    Expr *value; /* enum name: its explicit value, or NULL */
    Symbol *symbol; /* enum name; set by unit_analyze */
} Member;

/* The ".field" steps that follow the variable, or its element, in the
   target of an assignment or a read. */
typedef struct {
    Str name;
    SrcLoc loc;
    int index; /* the field's position; set by unit_analyze */
} PlaceField;

typedef struct {
    Stmt **stmts;
    size_t count;
    SrcLoc open;  /* the '{' */
    SrcLoc close; /* the '}' */
} Block;

typedef struct {
    TypeKind type; /* for an array parameter, the type of its elements */
    TypeRef *type_ref; /* NULL for a built-in type; resolved into "type" */
    Str name;
    SrcLoc loc;
    bool is_slice;  /* declared as "T name[]" */
    Symbol *symbol; /* set by unit_analyze */
} Param;

/* A variable that "cin" reads into: an existing one, or one that the read
   itself declares ("cin >> int n"). */
typedef struct {
    Str name;
    SrcLoc loc;
    bool declares;
    /* The declared type when declares is set; else the type of the place
       read into, set by unit_analyze. */
    TypeKind type;
    TypeRef *type_ref;    /* as for Param */
    Expr *index;          /* set for "cin >> name[index]" */
    PlaceField *fields;   /* set for "cin >> name.field" */
    size_t field_count;
    const Symbol *symbol; /* set by unit_analyze; NULL if it cannot be read into */
} ReadTarget;

struct Stmt {
    StmtKind kind;
    /* The declared, assigned or jumped-to name where there is one, else the
       statement's keyword. */
    SrcLoc loc;
    /* The first and last tokens of a statement that sits in a block. */
    SrcLoc start;
    SrcLoc end;
    /* STMT_CONST, STMT_FUNC, STMT_TYPE: declared with "export". */
    bool exported;
    union {
        /* STMT_CONST, STMT_VAR */
        struct {
            TypeKind type; /* for an array, the type of its elements */
            TypeRef *type_ref; /* NULL for a built-in type; resolved into "type" */
            Str name;
            Expr *init; /* NULL for a variable left at its zero value */
            Symbol *symbol;
            /* "T name[length] = { elems }". Elements left out are zero. */
            bool is_array;
            Expr *length; /* NULL when the length comes from the element count */
            Expr **elems;
            size_t elem_count;
        } decl;
        /* STMT_ASSIGN. "x += v", "x++" and "x--" are compound: the variable
           is combined with value using op. */
        struct {
            Str name;
            const Symbol *symbol;
            Expr *value;
            bool compound;
            Op op;
            Expr *index; /* set for "name[index] = value" */
            PlaceField *fields; /* set for "name.field = value" */
            size_t field_count;
            TypeKind type; /* of the place assigned; set by unit_analyze */
        } assign;
        /* STMT_EXPR: always a call */
        Expr *expr;
        /* STMT_COUT */
        struct {
            Expr **args;
            size_t count;
        } cout;
        /* STMT_CIN */
        struct {
            ReadTarget *targets;
            size_t count;
        } cin;
        /* STMT_IF. A body written without braces is a block holding its one
           statement, which is how "else if" appears. */
        struct {
            Expr *cond;
            Block then_block;
            Block else_block;
            bool has_else;
        } if_;
        /* STMT_WHILE, STMT_DO_WHILE, STMT_FOR */
        struct {
            Stmt *init; /* for only; NULL if left out */
            Expr *cond; /* NULL in a for loop without one: loop forever */
            Stmt *step; /* for only; NULL if left out */
            Block body;
            int id; /* set by unit_analyze; unique among the unit's loops */
        } loop;
        /* STMT_BREAK, STMT_CONTINUE: the loop it applies to; set by
           unit_analyze, NULL if it is not inside one. */
        const Stmt *target;
        /* STMT_GOTO, STMT_LABEL */
        struct {
            Str name;
            Symbol *symbol;
        } label;
        /* STMT_RETURN */
        struct {
            Expr *value; /* NULL for a bare return */
        } ret;
        /* STMT_FUNC */
        struct {
            TypeKind ret;
            TypeRef *ret_ref; /* NULL for a built-in type; resolved into "ret" */
            Str name;
            Param *params;
            size_t param_count;
            Block body;
            Symbol *symbol;
            SymbolList locals; /* variables declared anywhere in the body */
        } func;
        /* STMT_TYPE: "struct Name { fields }", "enum Name { names }" or
           "typedef type Name". C's "typedef struct Tag { ... } Name" declares
           one type under both names. */
        struct {
            TypeDeclKind kind;
            Str name;
            Str tag; /* the second name, or empty */
            SrcLoc tag_loc;
            Member *members;
            size_t member_count;
            TypeKind type; /* alias: the type named; else set by unit_analyze */
            TypeRef *type_ref;
            Symbol *symbol;
        } type;
        /* STMT_IMPORT: import "path" [as alias] */
        struct {
            Str path; /* as written */
            Str alias; /* empty without "as" */
            SrcLoc alias_loc;
            Symbol *symbol; /* set by unit_analyze; NULL if the name is unusable */
        } import;
    } as;
};

typedef struct {
    Block top;
    SymbolList globals; /* variables declared directly at the top level */
    SymbolList locals;  /* variables declared in blocks of top-level code */
    /* The function named main, which makes the file a C-style program: the
       top level may only hold declarations, and main is the entry point.
       NULL for a file whose top-level statements are the program. */
    const Stmt *main;
} Program;

/* One analyzed source file. Everything it owns lives in its arena.

   The unit handed to unit_analyze is the root: the program being compiled.
   The files it imports, directly or through other imports, are units too,
   owned by the root and each analyzed once however often it is imported. */
struct Unit {
    Arena arena;
    Str src;
    Diags diags;
    Program program;
    SymbolList symbols; /* every symbol, in declaration order */
    Ref *refs;
    size_t ref_count;
    size_t ref_cap;

    /* Where the file is, spelled so that it can be opened and shown to the
       user; NULL for source that is not in a file. */
    const char *path;
    /* The canonical form of path, which identifies the file; NULL if the
       file does not exist. */
    const char *real_path;
    Unit *root; /* itself for the root */
    /* 0 for the root, and unique among its modules; keeps the generated C
       names of different files apart. */
    int id;
    bool loading; /* its imports are still being followed */
    /* Root only: every imported file. One whose analysis has finished comes
       after all the files it imports. */
    Unit **modules;
    size_t module_count;
    size_t module_cap;
    /* Root only: the structs and enums declared in any of the program's
       files. */
    TypeTable types;
};

/* Parses and checks src, which must outlive the unit. path is the file it
   came from, against which its imports are resolved, or NULL. Never fails:
   problems are collected in unit->diags (sorted by position), and the
   program holds every statement that could be parsed. A file's problems are
   in its own unit; an import of a file with errors is itself an error. Only
   a unit with no errors may be passed to emit_c. */
void unit_analyze(Unit *unit, Str src, const char *path);
void unit_release(Unit *unit);

const char *op_text(Op op);
bool op_is_shift(Op op);

Program parse_program(Str src, Arena *arena, Diags *diags);
void check_program(Unit *unit);

#endif
