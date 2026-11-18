/* Kth largest; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_K 100000

static int64_t heap[MAX_K];
static int64_t size = 0;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void push(int64_t value) {
    int64_t at = size++;
    heap[at] = value;
    while (at > 0) {
        int64_t parent = (at - 1) / 2;
        if (heap[parent] <= heap[at]) break;
        int64_t held = heap[parent];
        heap[parent] = heap[at];
        heap[at] = held;
        at = parent;
    }
}

static void replace_root(int64_t value) {
    heap[0] = value;
    int64_t at = 0;
    for (;;) {
        int64_t child = 2 * at + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1] < heap[child]) child++;
        if (heap[at] <= heap[child]) break;
        int64_t held = heap[child];
        heap[child] = heap[at];
        heap[at] = held;
        at = child;
    }
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t n = bench ? 30000000 : 10000;
    int64_t k = bench ? MAX_K : 100;
    for (int64_t i = 0; i < n; i++) {
        int64_t value = random_number();
        if (size < k) push(value);
        else if (value > heap[0]) replace_root(value);
    }
    int64_t sum = 0;
    for (int64_t i = 0; i < size; i++) sum += heap[i];
    printf("sum %lld, k-th largest %lld\n", (long long)sum, (long long)heap[0]);
    return 0;
}
