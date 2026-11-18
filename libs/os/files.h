#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/* Part of ::os, in C: files and folders. A Wind library carries this
   header, and the program it is built into holds its text after Wind's own
   types, which it uses. */

#ifndef WIND_PROGRAM
#include "../wind.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WIND_OS_COMMON
#define WIND_OS_COMMON
/* A copy of a string with a '\0' after it, which C's functions need. */
static char *os_cstring(wind_string s) {
    char *copy = malloc(s.len + 1);
    if (!copy) {
        fputs("wind: out of memory\n", stderr);
        exit(1);
    }
    if (s.len) memcpy(copy, s.data, s.len);
    copy[s.len] = '\0';
    return copy;
}

/* Adds count bytes to the end of an array of char. */
static void os_append(wind_list_char *into, const char *bytes, size_t count) {
    if (into->len + count > into->cap) {
        size_t room = into->cap ? into->cap : 64;
        while (room < into->len + count) room *= 2;
        wind_char *grown = realloc(into->data, room);
        if (!grown) {
            fputs("wind: out of memory\n", stderr);
            exit(1);
        }
        into->data = grown;
        into->cap = room;
    }
    if (count) memcpy(into->data + into->len, bytes, count);
    into->len += count;
}
#endif

/* Appends the whole of a file to into. False if it cannot be read. */
static wind_bool os_read_file(wind_string path, wind_list_char *into) {
    char *name = os_cstring(path);
    FILE *f = fopen(name, "rb");
    free(name);
    if (!f) return 0;
    char buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, f)) > 0) os_append(into, buffer, got);
    wind_bool ok = !ferror(f);
    fclose(f);
    return ok;
}

/* Writes text as the whole of a file, or at its end with add. False if
   it cannot be written. */
static wind_bool os_write_file(wind_string path, wind_string text, wind_bool add) {
    char *name = os_cstring(path);
    FILE *f = fopen(name, add ? "ab" : "wb");
    free(name);
    if (!f) return 0;
    wind_bool ok = text.len == 0 || fwrite(text.data, 1, text.len, f) == text.len;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* What is at path: 0 nothing, 1 a file, 2 a folder, 3 anything else. */
static wind_int os_kind(wind_string path) {
    char *name = os_cstring(path);
    struct stat info;
    int found = stat(name, &info);
    free(name);
    if (found != 0) return 0;
    if (S_ISDIR(info.st_mode)) return 2;
    return S_ISREG(info.st_mode) ? 1 : 3;
}

/* Makes the folder at path, and those it is in that are not there yet.
   True if it is there afterwards. */
static wind_bool os_make_dirs(wind_string path) {
    char *name = os_cstring(path);
    for (char *at = name + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        mkdir(name, 0777);
        *at = '/';
    }
    mkdir(name, 0777);
    struct stat info;
    wind_bool ok = stat(name, &info) == 0 && S_ISDIR(info.st_mode);
    free(name);
    return ok;
}

/* Removes a file, or an empty folder. True if it was removed. */
static wind_bool os_remove(wind_string path) {
    char *name = os_cstring(path);
    struct stat info;
    int found = stat(name, &info);
    wind_bool ok = found == 0 && (S_ISDIR(info.st_mode) ? rmdir(name) : unlink(name)) == 0;
    free(name);
    return ok;
}

static int os_compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Appends to into the names of what is in a folder, in byte order, "." and
   ".." left out. The names' characters live until the program ends. False
   if the folder cannot be read. */
static wind_bool os_list_dir(wind_string path, wind_list_string *into) {
    char *name = os_cstring(path);
    DIR *folder = opendir(name);
    free(name);
    if (!folder) return 0;
    char **names = NULL;
    size_t count = 0, cap = 0;
    for (struct dirent *entry; (entry = readdir(folder));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            char **grown = realloc(names, cap * sizeof *names);
            if (!grown) break;
            names = grown;
        }
        names[count] = malloc(strlen(entry->d_name) + 1);
        if (!names[count]) break;
        strcpy(names[count++], entry->d_name);
    }
    closedir(folder);
    if (count) qsort(names, count, sizeof *names, os_compare_names);
    for (size_t i = 0; i < count; i++) {
        if (into->len == into->cap) {
            size_t room = into->cap ? into->cap * 2 : 16;
            wind_string *grown = realloc(into->data, room * sizeof *grown);
            if (!grown) break;
            into->data = grown;
            into->cap = room;
        }
        wind_string entry = {names[i], strlen(names[i])};
        into->data[into->len++] = entry;
    }
    free(names);
    return 1;
}

/* Appends the folder the program is working in to into. */
static wind_bool os_current_dir(wind_list_char *into) {
    char *here = getcwd(NULL, 0);
    if (!here) return 0;
    os_append(into, here, strlen(here));
    free(here);
    return 1;
}
