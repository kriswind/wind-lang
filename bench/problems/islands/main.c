/* Counting islands; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_SIDE 3000
#define MAX (MAX_SIDE * MAX_SIDE)

/* 0 is water, 1 unvisited land, 2 land already counted. */
static uint8_t land[MAX];
static int64_t stack[MAX];
static int64_t top = 0;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void visit(int64_t cell) {
    if (land[cell] == 1) {
        land[cell] = 2;
        stack[top++] = cell;
    }
}

int main(int argc, char **argv) {
    int64_t side = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX_SIDE : 60;
    int64_t n = side * side;
    for (int64_t i = 0; i < n; i++) land[i] = random_number() % 100 < 45 ? 1 : 0;
    int64_t islands = 0, largest = 0;
    for (int64_t start = 0; start < n; start++) {
        if (land[start] != 1) continue;
        islands++;
        int64_t size = 0;
        visit(start);
        while (top > 0) {
            int64_t cell = stack[--top];
            size++;
            int64_t row = cell / side, col = cell % side;
            if (row > 0) visit(cell - side);
            if (row < side - 1) visit(cell + side);
            if (col > 0) visit(cell - 1);
            if (col < side - 1) visit(cell + 1);
        }
        if (size > largest) largest = size;
    }
    printf("%lld islands, the largest of %lld cells\n", (long long)islands, (long long)largest);
    return 0;
}
