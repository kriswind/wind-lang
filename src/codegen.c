#include "codegen.h"

static const char prelude[] =
    "#include <stddef.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "typedef struct {\n"
    "    const char *data;\n"
    "    size_t len;\n"
    "} wind_string;\n"
    "\n"
    "static void wind_write(wind_string s) {\n"
    "    fwrite(s.data, 1, s.len, stdout);\n"
    "}\n"
    "\n";

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

static void emit_name(Str name, FILE *out) {
    fprintf(out, "wind_%.*s", (int)name.len, name.data);
}

static void emit_const_decl(const Stmt *stmt, FILE *out) {
    /* Constants are compile-time values: an initializer that names another
       constant is folded down to its literal, since C does not accept one
       static object as the initializer of another. */
    Str value = const_decl_literal(stmt)->as.string_lit;
    fputs("static const wind_string ", out);
    emit_name(stmt->as.const_decl.name, out);
    fputs(" = { ", out);
    emit_c_string(value, out);
    fprintf(out, ", %zu };\n", value.len);
}

static void emit_expr(const Expr *expr, FILE *out) {
    switch (expr->kind) {
    case EXPR_STRING_LIT:
        fputs("(wind_string){ ", out);
        emit_c_string(expr->as.string_lit, out);
        fprintf(out, ", %zu }", expr->as.string_lit.len);
        break;
    case EXPR_IDENT:
        emit_name(expr->as.ident.name, out);
        break;
    }
}

void emit_c(const Program *program, FILE *out) {
    fputs(prelude, out);

    for (size_t i = 0; i < program->count; i++) {
        if (program->stmts[i]->kind == STMT_CONST_DECL) {
            emit_const_decl(program->stmts[i], out);
        }
    }

    fputs("\nint main(void) {\n", out);
    for (size_t i = 0; i < program->count; i++) {
        const Stmt *stmt = program->stmts[i];
        if (stmt->kind != STMT_COUT) continue;
        for (size_t j = 0; j < stmt->as.cout.count; j++) {
            fputs("    wind_write(", out);
            emit_expr(stmt->as.cout.args[j], out);
            fputs(");\n", out);
        }
    }
    fputs("    return 0;\n}\n", out);
}
