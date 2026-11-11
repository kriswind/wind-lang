#ifndef WIND_CODEGEN_H
#define WIND_CODEGEN_H

#include <stdio.h>

#include "ast.h"

/* Writes a self-contained C translation unit for a unit that analyzed
   without errors. */
void emit_c(const Unit *unit, FILE *out);

#endif
