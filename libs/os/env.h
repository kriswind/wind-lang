#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/* Part of ::os, in C: the environment. A Wind library carries this header,
   and the program it is built into holds its text after Wind's own types,
   which it uses. */

#ifndef WIND_PROGRAM
#include "../wind.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A copy of a string with a '\0' after it, kept for setenv. */
static char *os_env_cstring(wind_string s) {
    char *copy = malloc(s.len + 1);
    if (!copy) {
        fputs("wind: out of memory\n", stderr);
        exit(1);
    }
    if (s.len) memcpy(copy, s.data, s.len);
    copy[s.len] = '\0';
    return copy;
}

/* The value of an environment variable, or "" if it is not set; with set,
   whether it is. The value is a view that a later os_set_env of the same
   name may end. */
static wind_string os_get_env(wind_string name, wind_bool *set) {
    char *key = os_env_cstring(name);
    const char *value = getenv(key);
    free(key);
    if (set) *set = value != NULL;
    wind_string text = {value ? value : "", value ? strlen(value) : 0};
    return text;
}

/* Sets an environment variable, which the programs this one runs see. */
static wind_bool os_set_env(wind_string name, wind_string value) {
    char *key = os_env_cstring(name);
    char *text = os_env_cstring(value);
    wind_bool ok = setenv(key, text, 1) == 0;
    free(key);
    free(text);
    return ok;
}
