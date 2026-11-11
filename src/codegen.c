#define _POSIX_C_SOURCE 200809L /* open_memstream */

#include "codegen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Shape of the generated C:
 *
 * - Top-level statements become the body of the C main(), which then calls
 *   the Wind main if there is one. Functions become static C functions.
 * - Variables declared directly at the top level are static objects. Every
 *   other variable is declared at the top of its C function. Either way it
 *   starts zeroed and its Wind declaration becomes an assignment, so a goto
 *   can never jump past an initialization into undefined state.
 * - Non-string constants are substituted at each use. String constants are
 *   static objects.
 * - Arithmetic results are cast back to their Wind type, undoing C's integer
 *   promotion so that narrow types wrap at their own width.
 *
 * - Only the runtime support a program uses is emitted. The program is
 *   generated first, into memory, recording each runtime type and helper it
 *   mentions; the matching definitions are then written ahead of it.
 *
 * Generated names are prefixed by kind and, for locals, numbered, so no two
 * Wind names can meet in C.
 */

/* ---- Runtime support ---- */

typedef enum {
    HELPER_WRITE_STRING,
    HELPER_STRING_EQ,
    HELPER_WRITE_BOOL,
    HELPER_WRITE_CHAR,
    HELPER_WRITE_INT,
    HELPER_WRITE_UINT,
    HELPER_WRITE_U128,
    HELPER_WRITE_I128,
    HELPER_WRITE_F16,
    HELPER_WRITE_F32,
    HELPER_WRITE_F64,
    HELPER_WRITE_F128,
    HELPER_COUNT,
} Helper;

/* What the program generated so far needs from the runtime. */
static struct {
    bool types[TYPE_COUNT];
    bool helpers[HELPER_COUNT];
} used;

/* The C name of a type, noted as needing its definition. */
static const char *c_type(TypeKind type) {
    used.types[type] = true;
    return type_c_name(type);
}

static const char *const type_definitions[TYPE_COUNT] = {
    [TYPE_STRING] = "typedef struct {\n"
                    "    const char *data;\n"
                    "    size_t len;\n"
                    "} wind_string;\n",
    [TYPE_BOOL] = "typedef _Bool wind_bool;\n",
    [TYPE_CHAR] = "typedef unsigned char wind_char;\n",
    [TYPE_INT] = "typedef intptr_t wind_int;\n",
    [TYPE_I8] = "typedef int8_t wind_i8;\n",
    [TYPE_I16] = "typedef int16_t wind_i16;\n",
    [TYPE_I32] = "typedef int32_t wind_i32;\n",
    [TYPE_I64] = "typedef int64_t wind_i64;\n",
    [TYPE_I128] = "typedef __int128 wind_i128;\n",
    [TYPE_UINT] = "typedef uintptr_t wind_uint;\n",
    [TYPE_U8] = "typedef uint8_t wind_u8;\n",
    [TYPE_U16] = "typedef uint16_t wind_u16;\n",
    [TYPE_U32] = "typedef uint32_t wind_u32;\n",
    [TYPE_U64] = "typedef uint64_t wind_u64;\n",
    [TYPE_U128] = "typedef unsigned __int128 wind_u128;\n",
    [TYPE_UINTPTR] = "typedef uintptr_t wind_uintptr;\n",
    [TYPE_FLOAT] = "typedef double wind_float;\n",
    [TYPE_F32] = "typedef float wind_f32;\n",
    [TYPE_F64] = "typedef double wind_f64;\n",
    /* The two types below fall back to the nearest standard type on
       compilers without the extension. */
    [TYPE_F16] = "#ifdef __FLT16_MANT_DIG__\n"
                 "typedef _Float16 wind_f16;\n"
                 "#else\n"
                 "typedef float wind_f16;\n"
                 "#endif\n",
    [TYPE_F128] = "#ifdef __SIZEOF_FLOAT128__\n"
                  "typedef __float128 wind_f128;\n"
                  "#else\n"
                  "typedef long double wind_f128;\n"
                  "#endif\n",
};

/* Defines the function that prints one float type. Prints the fewest digits
   that read back as the same value; values are formatted through long
   double, the widest type printf accepts. */
static const char float_writer_macro[] =
    "#define WIND_WRITE_FLOAT(name, T, max_digits, parse) \\\n"
    "    static void name(T v) { \\\n"
    "        char buf[64]; \\\n"
    "        T shown = (T)(long double)v; \\\n"
    "        for (int digits = 1; digits <= max_digits; digits++) { \\\n"
    "            snprintf(buf, sizeof buf, \"%.*Lg\", digits, (long double)v); \\\n"
    "            if ((T)parse(buf, NULL) == shown) break; \\\n"
    "        } \\\n"
    "        fputs(buf, stdout); \\\n"
    "    }\n";

static const char *const helper_definitions[HELPER_COUNT] = {
    [HELPER_WRITE_STRING] = "static void wind_write_string(wind_string s) {\n"
                            "    if (s.len) fwrite(s.data, 1, s.len, stdout);\n"
                            "}\n",
    [HELPER_STRING_EQ] =
        "static wind_bool wind_string_eq(wind_string a, wind_string b) {\n"
        "    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n"
        "}\n",
    [HELPER_WRITE_BOOL] = "static void wind_write_bool(wind_bool b) {\n"
                          "    fputs(b ? \"true\" : \"false\", stdout);\n"
                          "}\n",
    [HELPER_WRITE_CHAR] = "static void wind_write_char(wind_char c) {\n"
                          "    fputc(c, stdout);\n"
                          "}\n",
    [HELPER_WRITE_INT] = "static void wind_write_int(long long v) {\n"
                         "    printf(\"%lld\", v);\n"
                         "}\n",
    [HELPER_WRITE_UINT] = "static void wind_write_uint(unsigned long long v) {\n"
                          "    printf(\"%llu\", v);\n"
                          "}\n",
    [HELPER_WRITE_U128] = "static void wind_write_u128(wind_u128 v) {\n"
                          "    char buf[40];\n"
                          "    char *p = buf + sizeof buf;\n"
                          "    do {\n"
                          "        *--p = (char)('0' + (int)(v % 10));\n"
                          "        v /= 10;\n"
                          "    } while (v);\n"
                          "    fwrite(p, 1, (size_t)(buf + sizeof buf - p), stdout);\n"
                          "}\n",
    [HELPER_WRITE_I128] = "static void wind_write_i128(wind_i128 v) {\n"
                          "    if (v < 0) {\n"
                          "        fputc('-', stdout);\n"
                          "        wind_write_u128(-(wind_u128)v);\n"
                          "    } else {\n"
                          "        wind_write_u128((wind_u128)v);\n"
                          "    }\n"
                          "}\n",
    [HELPER_WRITE_F16] = "WIND_WRITE_FLOAT(wind_write_f16, wind_f16, 5, strtof)\n",
    [HELPER_WRITE_F32] = "WIND_WRITE_FLOAT(wind_write_f32, wind_f32, 9, strtof)\n",
    [HELPER_WRITE_F64] = "WIND_WRITE_FLOAT(wind_write_f64, wind_f64, 17, strtod)\n",
    [HELPER_WRITE_F128] = "WIND_WRITE_FLOAT(wind_write_f128, wind_f128, 21, strtold)\n",
};

/* The types each helper's own definition mentions. */
static const struct {
    Helper helper;
    TypeKind type;
} helper_types[] = {
    {HELPER_WRITE_STRING, TYPE_STRING}, {HELPER_STRING_EQ, TYPE_STRING},
    {HELPER_STRING_EQ, TYPE_BOOL},      {HELPER_WRITE_BOOL, TYPE_BOOL},
    {HELPER_WRITE_CHAR, TYPE_CHAR},     {HELPER_WRITE_U128, TYPE_U128},
    {HELPER_WRITE_I128, TYPE_I128},     {HELPER_WRITE_I128, TYPE_U128},
    {HELPER_WRITE_F16, TYPE_F16},       {HELPER_WRITE_F32, TYPE_F32},
    {HELPER_WRITE_F64, TYPE_F64},       {HELPER_WRITE_F128, TYPE_F128},
};

static bool any_used(const bool *flags, int first, int last) {
    for (int i = first; i <= last; i++) {
        if (flags[i]) return true;
    }
    return false;
}

/* Writes the headers, type definitions and helpers recorded in `used`. */
static void emit_runtime(FILE *out) {
    /* The signed 128-bit printer is written in terms of the unsigned one. */
    if (used.helpers[HELPER_WRITE_I128]) used.helpers[HELPER_WRITE_U128] = true;
    for (size_t i = 0; i < sizeof helper_types / sizeof helper_types[0]; i++) {
        if (used.helpers[helper_types[i].helper]) used.types[helper_types[i].type] = true;
    }

    bool floats = any_used(used.helpers, HELPER_WRITE_F16, HELPER_WRITE_F128);
    bool output = any_used(used.helpers, HELPER_WRITE_STRING, HELPER_WRITE_STRING) ||
                  any_used(used.helpers, HELPER_WRITE_BOOL, HELPER_WRITE_F128);
    bool fixed_ints = any_used(used.types, TYPE_INT, TYPE_I64) ||
                      any_used(used.types, TYPE_UINT, TYPE_U64) ||
                      used.types[TYPE_UINTPTR];

    bool any = false;
    if (used.types[TYPE_STRING] || used.helpers[HELPER_WRITE_U128]) {
        fputs("#include <stddef.h>\n", out);
        any = true;
    }
    if (fixed_ints) fputs("#include <stdint.h>\n", out);
    if (output) fputs("#include <stdio.h>\n", out);
    if (floats) fputs("#include <stdlib.h>\n", out);
    if (used.helpers[HELPER_STRING_EQ]) fputs("#include <string.h>\n", out);
    if (any || fixed_ints || output || floats) fputs("\n", out);

    any = false;
    for (int type = 0; type < TYPE_COUNT; type++) {
        if (!used.types[type] || !type_definitions[type]) continue;
        fputs(type_definitions[type], out);
        any = true;
    }
    if (any) fputs("\n", out);

    if (floats) fprintf(out, "%s\n", float_writer_macro);
    for (int helper = 0; helper < HELPER_COUNT; helper++) {
        if (used.helpers[helper]) fprintf(out, "%s\n", helper_definitions[helper]);
    }
}

/* ---- Program ---- */

static void emit_expr(const Expr *expr, FILE *out);

/* Non-printable bytes use three-digit octal escapes, which cannot absorb a
   following digit the way hex escapes do. '?' is escaped to rule out
   trigraphs. */
static void emit_c_string(Str s, FILE *out) {
    fputc('"', out);
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '"' || c == '\\' || c == '?') {
            fprintf(out, "\\%c", c);
        } else if (c >= 0x20 && c < 0x7f) {
            fputc(c, out);
        } else {
            fprintf(out, "\\%03o", c);
        }
    }
    fputc('"', out);
}

static void emit_symbol(const Symbol *symbol, FILE *out) {
    int len = (int)symbol->name.len;
    const char *name = symbol->name.data;
    switch (symbol->kind) {
    case SYM_FUNC:  fprintf(out, "wind_f_%.*s", len, name); break;
    case SYM_LABEL: fprintf(out, "wind_l_%.*s", len, name); break;
    case SYM_CONST: fprintf(out, "wind_c%d_%.*s", symbol->id, len, name); break;
    case SYM_VAR:
    case SYM_PARAM: fprintf(out, "wind_v%d_%.*s", symbol->id, len, name); break;
    }
}

/* The string literal a string constant ultimately names, following
   constant-to-constant initializers. */
static Str const_string(const Symbol *symbol) {
    const Expr *expr = symbol->decl->as.decl.init;
    while (expr->kind == EXPR_IDENT) expr = expr->as.ident.symbol->decl->as.decl.init;
    return expr->as.string_lit;
}

static void emit_int_literal(const Expr *expr, FILE *out) {
    U128 value = expr->as.int_lit;
    uint64_t low = (uint64_t)value;
    uint64_t high = (uint64_t)(value >> 64);
    fprintf(out, "((%s)", c_type(expr->type));
    if (high == 0) {
        fprintf(out, "%lluULL)", (unsigned long long)low);
    } else {
        fprintf(out, "(((%s)%lluULL << 64) | %lluULL))", c_type(TYPE_U128),
                (unsigned long long)high,
                (unsigned long long)low);
    }
}

static void emit_float_literal(const Expr *expr, FILE *out) {
    /* The suffix makes C read the digits at the precision of the target
       type instead of rounding through double. */
    const char *suffix = "";
    if (expr->type == TYPE_F16 || expr->type == TYPE_F32) suffix = "f";
    if (expr->type == TYPE_F128) suffix = "L";
    fprintf(out, "((%s)%.*s%s)", c_type(expr->type), (int)expr->as.float_lit.len,
            expr->as.float_lit.data, suffix);
}

static void emit_binary(const Expr *expr, FILE *out) {
    Op op = expr->as.binary.op;
    const Expr *left = expr->as.binary.left;
    const Expr *right = expr->as.binary.right;

    if (left->type == TYPE_STRING) {
        used.helpers[HELPER_STRING_EQ] = true;
        fputs(op == OP_NE ? "(!wind_string_eq(" : "(wind_string_eq(", out);
        emit_expr(left, out);
        fputs(", ", out);
        emit_expr(right, out);
        fputs("))", out);
        return;
    }
    fprintf(out, "((%s)(", c_type(expr->type));
    emit_expr(left, out);
    fprintf(out, " %s ", op_text(op));
    emit_expr(right, out);
    fputs("))", out);
}

static void emit_expr(const Expr *expr, FILE *out) {
    switch (expr->kind) {
    case EXPR_INT_LIT:
        emit_int_literal(expr, out);
        break;
    case EXPR_FLOAT_LIT:
        emit_float_literal(expr, out);
        break;
    case EXPR_CHAR_LIT:
        fprintf(out, "((%s)%d)", c_type(TYPE_CHAR), (unsigned char)expr->as.char_lit);
        break;
    case EXPR_STRING_LIT:
        fprintf(out, "(%s){ ", c_type(TYPE_STRING));
        emit_c_string(expr->as.string_lit, out);
        fprintf(out, ", %zu }", expr->as.string_lit.len);
        break;
    case EXPR_BOOL_LIT:
        fprintf(out, "((%s)%d)", c_type(TYPE_BOOL), expr->as.bool_lit ? 1 : 0);
        break;
    case EXPR_IDENT: {
        const Symbol *symbol = expr->as.ident.symbol;
        if (symbol->kind == SYM_CONST && symbol->type != TYPE_STRING) {
            fprintf(out, "((%s)", c_type(symbol->type));
            emit_expr(symbol->decl->as.decl.init, out);
            fputs(")", out);
        } else {
            emit_symbol(symbol, out);
        }
        break;
    }
    case EXPR_UNARY:
        fprintf(out, "((%s)(%s", c_type(expr->type), op_text(expr->as.unary.op));
        emit_expr(expr->as.unary.operand, out);
        fputs("))", out);
        break;
    case EXPR_BINARY:
        emit_binary(expr, out);
        break;
    case EXPR_CALL:
        emit_symbol(expr->as.call.symbol, out);
        fputs("(", out);
        for (size_t i = 0; i < expr->as.call.count; i++) {
            if (i) fputs(", ", out);
            emit_expr(expr->as.call.args[i], out);
        }
        fputs(")", out);
        break;
    case EXPR_CAST:
        fprintf(out, "((%s)", c_type(expr->type));
        emit_expr(expr->as.cast.operand, out);
        fputs(")", out);
        break;
    case EXPR_SIZEOF:
        break; /* rewritten to an integer literal by unit_analyze */
    }
}

static void emit_write(const Expr *arg, FILE *out) {
    static const char *const names[HELPER_COUNT] = {
        [HELPER_WRITE_STRING] = "wind_write_string", [HELPER_WRITE_BOOL] = "wind_write_bool",
        [HELPER_WRITE_CHAR] = "wind_write_char",     [HELPER_WRITE_INT] = "wind_write_int",
        [HELPER_WRITE_UINT] = "wind_write_uint",     [HELPER_WRITE_U128] = "wind_write_u128",
        [HELPER_WRITE_I128] = "wind_write_i128",     [HELPER_WRITE_F16] = "wind_write_f16",
        [HELPER_WRITE_F32] = "wind_write_f32",       [HELPER_WRITE_F64] = "wind_write_f64",
        [HELPER_WRITE_F128] = "wind_write_f128",
    };
    TypeKind type = arg->type;
    Helper helper;
    const char *cast = "";
    switch (type) {
    case TYPE_STRING: helper = HELPER_WRITE_STRING; break;
    case TYPE_BOOL:   helper = HELPER_WRITE_BOOL;   break;
    case TYPE_CHAR:   helper = HELPER_WRITE_CHAR;   break;
    case TYPE_I128:   helper = HELPER_WRITE_I128;   break;
    case TYPE_U128:   helper = HELPER_WRITE_U128;   break;
    case TYPE_F16:    helper = HELPER_WRITE_F16;    break;
    case TYPE_F32:    helper = HELPER_WRITE_F32;    break;
    case TYPE_F128:   helper = HELPER_WRITE_F128;   break;
    case TYPE_FLOAT:
    case TYPE_F64:    helper = HELPER_WRITE_F64;    break;
    default:
        /* Every narrower integer is printed through the widest one. */
        helper = type_is_signed(type) ? HELPER_WRITE_INT : HELPER_WRITE_UINT;
        cast = type_is_signed(type) ? "(long long)" : "(unsigned long long)";
        break;
    }
    used.helpers[helper] = true;
    fprintf(out, "%s(%s", names[helper], cast);
    emit_expr(arg, out);
    fputs(");\n", out);
}

static void indent(int depth, FILE *out) {
    for (int i = 0; i < depth; i++) fputs("    ", out);
}

static void emit_block(const Block *block, int depth, FILE *out);

static void emit_stmt(const Stmt *stmt, int depth, FILE *out) {
    switch (stmt->kind) {
    case STMT_CONST:
    case STMT_FUNC:
        break;
    case STMT_VAR:
        indent(depth, out);
        emit_symbol(stmt->as.decl.symbol, out);
        fputs(" = ", out);
        if (stmt->as.decl.init) {
            emit_expr(stmt->as.decl.init, out);
        } else {
            fprintf(out, "(%s){0}", c_type(stmt->as.decl.type));
        }
        fputs(";\n", out);
        break;
    case STMT_ASSIGN:
        indent(depth, out);
        emit_symbol(stmt->as.assign.symbol, out);
        fputs(" = ", out);
        if (stmt->as.assign.compound) {
            fprintf(out, "((%s)(", c_type(stmt->as.assign.symbol->type));
            emit_symbol(stmt->as.assign.symbol, out);
            fprintf(out, " %s ", op_text(stmt->as.assign.op));
            emit_expr(stmt->as.assign.value, out);
            fputs("))", out);
        } else {
            emit_expr(stmt->as.assign.value, out);
        }
        fputs(";\n", out);
        break;
    case STMT_EXPR:
        indent(depth, out);
        emit_expr(stmt->as.expr, out);
        fputs(";\n", out);
        break;
    case STMT_COUT:
        for (size_t i = 0; i < stmt->as.cout.count; i++) {
            indent(depth, out);
            emit_write(stmt->as.cout.args[i], out);
        }
        break;
    case STMT_IF:
        indent(depth, out);
        fputs("if (", out);
        emit_expr(stmt->as.if_.cond, out);
        fputs(") {\n", out);
        emit_block(&stmt->as.if_.then_block, depth + 1, out);
        indent(depth, out);
        if (stmt->as.if_.has_else) {
            fputs("} else {\n", out);
            emit_block(&stmt->as.if_.else_block, depth + 1, out);
            indent(depth, out);
        }
        fputs("}\n", out);
        break;
    case STMT_GOTO:
        indent(depth, out);
        fputs("goto ", out);
        emit_symbol(stmt->as.label.symbol, out);
        fputs(";\n", out);
        break;
    case STMT_LABEL:
        /* The empty statement lets a label be the last thing in a block. */
        emit_symbol(stmt->as.label.symbol, out);
        fputs(":;\n", out);
        break;
    case STMT_RETURN:
        indent(depth, out);
        fputs("return", out);
        if (stmt->as.ret.value) {
            fputs(" ", out);
            emit_expr(stmt->as.ret.value, out);
        }
        fputs(";\n", out);
        break;
    }
}

static void emit_block(const Block *block, int depth, FILE *out) {
    for (size_t i = 0; i < block->count; i++) emit_stmt(block->stmts[i], depth, out);
}

static void emit_variables(const SymbolList *variables, const char *prefix, FILE *out) {
    for (size_t i = 0; i < variables->count; i++) {
        fprintf(out, "%s%s ", prefix, c_type(variables->items[i]->type));
        emit_symbol(variables->items[i], out);
        fputs(" = {0};\n", out);
    }
}

static void emit_signature(const Stmt *func, FILE *out) {
    fprintf(out, "static %s ", c_type(func->as.func.ret));
    emit_symbol(func->as.func.symbol, out);
    fputs("(", out);
    if (func->as.func.param_count == 0) fputs("void", out);
    for (size_t i = 0; i < func->as.func.param_count; i++) {
        if (i) fputs(", ", out);
        fprintf(out, "%s ", c_type(func->as.func.params[i].type));
        emit_symbol(func->as.func.params[i].symbol, out);
    }
    fputs(")", out);
}

static void emit_program(const Unit *unit, FILE *out) {
    const Block *top = &unit->program.top;
    bool any = false;

    for (size_t i = 0; i < unit->symbols.count; i++) {
        const Symbol *symbol = unit->symbols.items[i];
        if (symbol->kind != SYM_CONST || symbol->type != TYPE_STRING) continue;
        Str value = const_string(symbol);
        fprintf(out, "static const %s ", c_type(TYPE_STRING));
        emit_symbol(symbol, out);
        fputs(" = { ", out);
        emit_c_string(value, out);
        fprintf(out, ", %zu };\n", value.len);
        any = true;
    }
    emit_variables(&unit->program.globals, "static ", out);
    if (any || unit->program.globals.count) fputs("\n", out);

    /* Prototypes first, so functions can call each other in any order. */
    any = false;
    for (size_t i = 0; i < top->count; i++) {
        if (top->stmts[i]->kind != STMT_FUNC) continue;
        emit_signature(top->stmts[i], out);
        fputs(";\n", out);
        any = true;
    }
    if (any) fputs("\n", out);

    for (size_t i = 0; i < top->count; i++) {
        const Stmt *func = top->stmts[i];
        if (func->kind != STMT_FUNC) continue;
        emit_signature(func, out);
        fputs(" {\n", out);
        emit_variables(&func->as.func.locals, "    ", out);
        emit_block(&func->as.func.body, 1, out);
        fputs("}\n\n", out);
    }

    fputs("int main(void) {\n", out);
    emit_variables(&unit->program.locals, "    ", out);
    emit_block(top, 1, out);
    const Stmt *wind_main = unit->program.main;
    if (wind_main && wind_main->as.func.ret == TYPE_INT) {
        /* The value main returns is the process exit status. */
        fputs("    return (int)", out);
        emit_symbol(wind_main->as.func.symbol, out);
        fputs("();\n}\n", out);
        return;
    }
    if (wind_main) {
        fputs("    ", out);
        emit_symbol(wind_main->as.func.symbol, out);
        fputs("();\n", out);
    }
    fputs("    return 0;\n}\n", out);
}

void emit_c(const Unit *unit, FILE *out) {
    memset(&used, 0, sizeof used);

    char *program = NULL;
    size_t program_len = 0;
    FILE *buffer = open_memstream(&program, &program_len);
    if (!buffer) fatal("out of memory");
    emit_program(unit, buffer);
    if (fclose(buffer) != 0) fatal("out of memory");

    emit_runtime(out);
    fwrite(program, 1, program_len, out);
    free(program);
}
