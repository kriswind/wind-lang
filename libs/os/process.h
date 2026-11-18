#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/* Part of ::os, in C: running other programs. A Wind library carries this
   header, and the program it is built into holds its text after Wind's own
   types, which it uses. */

#ifndef WIND_PROGRAM
#include "../wind.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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

/* Runs command[0], found on PATH unless it has a '/', with the rest of
   command as its arguments, in the folder dir unless that is empty, and
   waits for it. With capture, what it writes to its output and its error
   output is appended to out and err; without, it shares the program's own.
   Returns its exit status, 128 plus the number of the signal that ended
   it, 127 if it could not be started, or -1 if no process could be made. */
static wind_int os_run(wind_slice_string command, wind_string dir, wind_list_char *out,
                       wind_list_char *err, wind_bool capture) {
    if (command.len == 0) return 127;
    char **argv = malloc((command.len + 1) * sizeof *argv);
    if (!argv) return -1;
    for (size_t i = 0; i < command.len; i++) argv[i] = os_cstring(command.data[i]);
    argv[command.len] = NULL;
    char *folder = dir.len ? os_cstring(dir) : NULL;

    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (capture && (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)) {
        for (size_t i = 0; i < command.len; i++) free(argv[i]);
        free(argv);
        free(folder);
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (capture) {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        if (folder && chdir(folder) != 0) {
            fprintf(stderr, "wind: cannot enter '%s': %s\n", folder, strerror(errno));
            _exit(127);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "wind: cannot run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    for (size_t i = 0; i < command.len; i++) free(argv[i]);
    free(argv);
    free(folder);
    if (pid < 0) {
        if (capture) {
            close(out_pipe[0]);
            close(out_pipe[1]);
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        return -1;
    }
    if (capture) {
        /* Both are read as the program writes them, so that neither fills
           up and leaves it waiting. */
        close(out_pipe[1]);
        close(err_pipe[1]);
        struct pollfd ends[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
        wind_list_char *into[2] = {out, err};
        int open_ends = 2;
        char buffer[4096];
        while (open_ends > 0) {
            if (poll(ends, 2, -1) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < 2; i++) {
                if (ends[i].fd < 0 || !(ends[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                    continue;
                }
                ssize_t got = read(ends[i].fd, buffer, sizeof buffer);
                if (got > 0) {
                    os_append(into[i], buffer, (size_t)got);
                } else if (got == 0 || errno != EINTR) {
                    close(ends[i].fd);
                    ends[i].fd = -1;
                    open_ends--;
                }
            }
        }
        for (int i = 0; i < 2; i++) {
            if (ends[i].fd >= 0) close(ends[i].fd);
        }
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

/* True if a program of this name would be found to run: on PATH, or at the
   path given if it has a '/'. */
static wind_bool os_on_path(wind_string name) {
    if (!name.len) return 0;
    char *wanted = os_cstring(name);
    if (strchr(wanted, '/')) {
        wind_bool found = access(wanted, X_OK) == 0;
        free(wanted);
        return found;
    }
    const char *path = getenv("PATH");
    wind_bool found = 0;
    while (path && *path && !found) {
        size_t len = strcspn(path, ":");
        char *candidate = malloc((len ? len : 1) + strlen(wanted) + 2);
        if (!candidate) break;
        sprintf(candidate, "%.*s/%s", len ? (int)len : 1, len ? path : ".", wanted);
        found = access(candidate, X_OK) == 0;
        free(candidate);
        path += len;
        if (*path == ':') path++;
    }
    free(wanted);
    return found;
}
