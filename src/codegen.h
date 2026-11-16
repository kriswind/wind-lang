#ifndef WIND_CODEGEN_H
#define WIND_CODEGEN_H

#include <stdio.h>

#include "ast.h"

typedef struct {
    /* Leave out the test that stops the program when an index is out of
       range, except in code marked "#bounds_check". A negative index still
       counts back from the end. */
    bool no_bounds_check;
} EmitOptions;

/* Writes a self-contained C translation unit for a unit that analyzed
   without errors. */
void emit_c(const Unit *unit, FILE *out, EmitOptions options);

#endif
