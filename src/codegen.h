#ifndef WIND_CODEGEN_H
#define WIND_CODEGEN_H

#include <stdio.h>

#include "ast.h"

struct Target;

typedef struct {
    /* What the program is compiled for; see targets/target.h. NULL is the
       machine the compiler runs on. */
    const struct Target *target;
    /* Leave out the test that stops the program when an index is out of
       range, except in code marked "#bounds_check". A negative index still
       counts back from the end. */
    bool no_bounds_check;
} EmitOptions;

/* Writes a self-contained C translation unit for a unit that analyzed
   without errors. The unit must have been analyzed for the platform of
   the target. */
void emit_c(const Unit *unit, FILE *out, EmitOptions options);

/* The C libraries that the C emit_c last wrote must be linked with, named
   as "-l" takes them: those of the external functions and constants it
   uses. The names are valid until emit_c is called again. */
const char *const *emit_c_links(size_t *count);

#endif
