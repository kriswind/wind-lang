#ifndef WIND_AST_H
#define WIND_AST_H

#include "common.h"
#include "types.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Symbol Symbol;

/* ---- Symbols ---- */

typedef enum {
    SYM_CONST,
    SYM_VAR,
    SYM_PARAM,
    SYM_FUNC,
    SYM_LABEL,
} SymbolKind;

/* A named thing a program declares. Created by unit_analyze. */
struct Symbol {
    SymbolKind kind;
    Str name;
    SrcLoc loc;    /* the name at its declaration */
    /* SYM_FUNC: the return type; SYM_LABEL: unused; an array: the type of
       its elements. */
    TypeKind type;
    /* An array is either fixed (declared with a length, which it owns) or a
       slice (an array parameter: a view of someone else's elements, with a
       length known only at run time). */
    bool is_array;
    bool is_slice;
    size_t length; /* fixed arrays only */
    /* Unique within the unit; keeps generated C names from colliding. */
    int id;
    /* The declaring statement: STMT_CONST, STMT_VAR or STMT_FUNC. NULL for
       parameters and labels. */
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
} Op;

struct Expr {
    ExprKind kind;
    /* The whole expression when it fits on one line, else its first token. */
    SrcLoc loc;
    TypeKind type; /* set by unit_analyze */
    union {
        U128 int_lit;
        Str float_lit; /* spelling, without digit separators */
        char char_lit;
        Str string_lit; /* decoded bytes */
        bool bool_lit;
        struct {
            Str name;
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
            const Symbol *symbol; /* set by unit_analyze; NULL if undefined */
            Expr **args;
            size_t count;
        } call;
        struct {
            TypeKind to;
            Expr *operand;
        } cast;
        struct {
            TypeKind type; /* used when operand is NULL */
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

/* ---- Statements ---- */

typedef enum {
    STMT_CONST,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_COUT,
    STMT_CIN,
    STMT_IF,
    STMT_GOTO,
    STMT_LABEL,
    STMT_RETURN,
    STMT_FUNC,
} StmtKind;

typedef struct {
    Stmt **stmts;
    size_t count;
    SrcLoc open;  /* the '{' */
    SrcLoc close; /* the '}' */
} Block;

typedef struct {
    TypeKind type; /* for an array parameter, the type of its elements */
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
    TypeKind type;        /* the declared type, when declares is set */
    Expr *index;          /* set for "cin >> name[index]" */
    const Symbol *symbol; /* set by unit_analyze; NULL if it cannot be read into */
} ReadTarget;

struct Stmt {
    StmtKind kind;
    /* The declared, assigned or jumped-to name where there is one, else the
       statement's keyword. */
    SrcLoc loc;
    union {
        /* STMT_CONST, STMT_VAR */
        struct {
            TypeKind type; /* for an array, the type of its elements */
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
        /* STMT_IF. "else if" is an else block holding one if statement. */
        struct {
            Expr *cond;
            Block then_block;
            Block else_block;
            bool has_else;
        } if_;
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
            Str name;
            Param *params;
            size_t param_count;
            Block body;
            Symbol *symbol;
            SymbolList locals; /* variables declared anywhere in the body */
        } func;
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

/* One analyzed source file. Everything it owns lives in its arena. */
typedef struct {
    Arena arena;
    Str src;
    Diags diags;
    Program program;
    SymbolList symbols; /* every symbol, in declaration order */
    Ref *refs;
    size_t ref_count;
    size_t ref_cap;
} Unit;

/* Parses and checks src, which must outlive the unit. Never fails: problems
   are collected in unit->diags (sorted by position), and the program holds
   every statement that could be parsed. Only a unit with no errors may be
   passed to emit_c. */
void unit_analyze(Unit *unit, Str src);
void unit_release(Unit *unit);

const char *op_text(Op op);

Program parse_program(Str src, Arena *arena, Diags *diags);
void check_program(Unit *unit);

#endif
