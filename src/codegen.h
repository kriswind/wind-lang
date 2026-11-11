#ifndef WIND_CODEGEN_H
#define WIND_CODEGEN_H

#include <stdio.h>

#include "ast.h"

/* Writes a self-contained C11 translation unit for a checked program. */
void emit_c(const Program *program, FILE *out);

#endif
