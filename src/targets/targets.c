/* The targets a program can be compiled for, by name. */

#include "target.h"

static const Target *const targets[] = {&target_host, &target_arduino};

const Target *target_find(Str name) {
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        if (str_eq_cstr(name, targets[i]->platform.name)) return targets[i];
    }
    return NULL;
}
