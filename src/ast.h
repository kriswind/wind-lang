#ifndef WIND_AST_H
#define WIND_AST_H

#include "common.h"

typedef enum {
    /* The type of an expression that failed to check. It is compatible with
       everything, so one mistake is reported once. */
    TYPE_INVALID,
    TYPE_STRING,
} TypeKind;

typedef struct Stmt Stmt;

typedef enum {
    EXPR_STRING_LIT,
    EXPR_IDENT,
} ExprKind;

typedef struct {
    ExprKind kind;
    SrcLoc loc;
    TypeKind type; /* set by unit_analyze */
    union {
        Str string_lit;
        struct {
            Str name;
            const Stmt *decl; /* set by unit_analyze; NULL if undefined */
        } ident;
    } as;
} Expr;

typedef enum {
    STMT_CONST_DECL,
    STMT_COUT,
} StmtKind;

struct Stmt {
    StmtKind kind;
    SrcLoc loc; /* const_decl: the declared name; cout: the keyword */
    union {
        struct {
            TypeKind type;
            Str name;
            Expr *init;
            bool used; /* set by unit_analyze */
        } const_decl;
        struct {
            Expr **args;
            size_t count;
        } cout;
    } as;
};

typedef struct {
    Stmt **stmts;
    size_t count;
} Program;

/* One analyzed source file. Everything it owns lives in its arena. */
typedef struct {
    Arena arena;
    Str src;
    Diags diags;
    Program program;
} Unit;

/* Parses and checks src, which must outlive the unit. Never fails: problems
   are collected in unit->diags (sorted by position), and the program holds
   every statement that could be parsed. Only a unit with no errors may be
   passed to emit_c. */
void unit_analyze(Unit *unit, Str src);
void unit_release(Unit *unit);

const char *type_name(TypeKind type);

/* The string literal a constant ultimately evaluates to, following
   constant-to-constant initializers. NULL if the chain hits an undefined
   name. */
const Expr *const_decl_literal(const Stmt *decl);

Program parse_program(Str src, Arena *arena, Diags *diags);
void check_program(Program *program, Arena *arena, Diags *diags);

#endif
