/* Dijkstra on a grid; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_SIDE 1500
#define MAX (MAX_SIDE * MAX_SIDE)

static int64_t cost[MAX], dist[MAX], heap[4 * MAX + 1];
static int64_t size = 0, n;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void push(int64_t key) {
    int64_t at = size++;
    while (at > 0) {
        int64_t parent = (at - 1) / 2;
        if (heap[parent] <= key) break;
        heap[at] = heap[parent];
        at = parent;
    }
    heap[at] = key;
}

static int64_t pop(void) {
    int64_t top = heap[0];
    int64_t last = heap[--size];
    int64_t at = 0;
    for (;;) {
        int64_t child = 2 * at + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1] < heap[child]) child++;
        if (last <= heap[child]) break;
        heap[at] = heap[child];
        at = child;
    }
    heap[at] = last;
    return top;
}

static void relax(int64_t cell, int64_t d) {
    if (d < dist[cell]) {
        dist[cell] = d;
        push(d * n + cell);
    }
}

int main(int argc, char **argv) {
    int64_t side = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX_SIDE : 40;
    n = side * side;
    for (int64_t i = 0; i < n; i++) {
        cost[i] = 1 + random_number() % 9;
        dist[i] = INT64_C(1) << 62;
    }
    dist[0] = cost[0];
    push(cost[0] * n);
    while (size > 0) {
        int64_t key = pop();
        int64_t d = key / n, cell = key % n;
        if (d > dist[cell]) continue;
        int64_t row = cell / side, col = cell % side;
        if (row > 0) relax(cell - side, d + cost[cell - side]);
        if (row < side - 1) relax(cell + side, d + cost[cell + side]);
        if (col > 0) relax(cell - 1, d + cost[cell - 1]);
        if (col < side - 1) relax(cell + 1, d + cost[cell + 1]);
    }
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += dist[i];
    printf("the cheapest path costs %lld; the costs to every cell sum to %lld\n",
           (long long)dist[n - 1], (long long)total);
    return 0;
}
