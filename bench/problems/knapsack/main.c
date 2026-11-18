/* 0/1 knapsack; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_ITEMS 1000
#define MAX_CAPACITY (MAX_ITEMS * 125)

static int64_t weight[MAX_ITEMS], value[MAX_ITEMS];
static int64_t best[MAX_CAPACITY + 1];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t knapsack(int64_t n, int64_t capacity) {
    for (int64_t c = 0; c <= capacity; c++) best[c] = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t w = weight[i], v = value[i];
        for (int64_t c = capacity; c >= w; c--) {
            int64_t taken = best[c - w] + v;
            if (taken > best[c]) best[c] = taken;
        }
    }
    return best[capacity];
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX_ITEMS : 60;
    int64_t capacity = n * 125;
    for (int64_t i = 0; i < n; i++) {
        weight[i] = 1 + random_number() % 1000;
        value[i] = 1 + random_number() % 1000;
    }
    printf("%lld\n", (long long)knapsack(n, capacity));
    return 0;
}
