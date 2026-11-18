#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/* Part of ::os, in C: the clock and the date. A Wind library carries this
   header, and the program it is built into holds its text after Wind's own
   types, which it uses. */

#ifndef WIND_PROGRAM
#include "../wind.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Seconds from some moment fixed while the program runs, on a clock that
   only goes forward: the time between two readings is how long passed. */
static wind_f64 os_clock(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (wind_f64)now.tv_sec + (wind_f64)now.tv_nsec / 1e9;
}

/* Appends today's date, where the program runs, as "2026-11-18". */
static void os_today(wind_list_char *into) {
    time_t now = time(NULL);
    struct tm local;
    char text[16];
    size_t len = 0;
    if (localtime_r(&now, &local)) len = strftime(text, sizeof text, "%Y-%m-%d", &local);
    if (into->len + len > into->cap) {
        size_t room = into->cap ? into->cap * 2 : 16;
        while (room < into->len + len) room *= 2;
        wind_char *grown = realloc(into->data, room);
        if (!grown) {
            fputs("wind: out of memory\n", stderr);
            exit(1);
        }
        into->data = grown;
        into->cap = room;
    }
    if (len) memcpy(into->data + into->len, text, len);
    into->len += len;
}

/* Waits for the given number of seconds. */
static void os_sleep(wind_f64 seconds) {
    if (seconds <= 0) return;
    struct timespec wait;
    wait.tv_sec = (time_t)seconds;
    wait.tv_nsec = (long)((seconds - (wind_f64)wait.tv_sec) * 1e9);
    while (nanosleep(&wait, &wait) != 0) {
    }
}
