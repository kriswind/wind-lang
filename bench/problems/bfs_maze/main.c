/* Breadth-first search through a maze; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_SIDE 2000
#define MAX (MAX_SIDE * MAX_SIDE)

static uint8_t wall[MAX];
static int64_t dist[MAX], queue[MAX];
static int64_t tail = 0;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void visit(int64_t cell, int64_t d) {
    if (wall[cell] == 0 && dist[cell] == -1) {
        dist[cell] = d;
        queue[tail++] = cell;
    }
}

int main(int argc, char **argv) {
    int64_t side = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX_SIDE : 100;
    int64_t n = side * side;
    for (int64_t i = 0; i < n; i++) {
        wall[i] = random_number() % 100 < 30 ? 1 : 0;
        dist[i] = -1;
    }
    int64_t open[6] = {0, 1, side, n - 1, n - 2, n - 1 - side};
    for (int i = 0; i < 6; i++) wall[open[i]] = 0;
    int64_t head = 0;
    dist[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        int64_t cell = queue[head++];
        int64_t d = dist[cell] + 1;
        int64_t row = cell / side, col = cell % side;
        if (row > 0) visit(cell - side, d);
        if (row < side - 1) visit(cell + side, d);
        if (col > 0) visit(cell - 1, d);
        if (col < side - 1) visit(cell + 1, d);
    }
    printf("%lld steps, %lld cells reached\n", (long long)dist[n - 1], (long long)tail);
    return 0;
}
