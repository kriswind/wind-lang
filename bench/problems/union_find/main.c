/* Union-find; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 2000000

static int64_t parent[MAX], size[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t find(int64_t node) {
    int64_t at = node;
    while (parent[at] != at) {
        parent[at] = parent[parent[at]];
        at = parent[at];
    }
    return at;
}

static bool join(int64_t a, int64_t b) {
    int64_t x = find(a), y = find(b);
    if (x == y) return false;
    if (size[x] < size[y]) {
        int64_t held = x;
        x = y;
        y = held;
    }
    parent[y] = x;
    size[x] += size[y];
    return true;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    for (int64_t i = 0; i < n; i++) {
        parent[i] = i;
        size[i] = 1;
    }
    int64_t groups = n, joins = n * 9 / 10;
    for (int64_t i = 0; i < joins; i++) {
        int64_t a = random_number() % n;
        int64_t b = random_number() % n;
        if (join(a, b)) groups--;
    }
    int64_t largest = 0;
    for (int64_t i = 0; i < n; i++) {
        if (parent[i] == i && size[i] > largest) largest = size[i];
    }
    printf("%lld groups, the largest of %lld\n", (long long)groups, (long long)largest);
    return 0;
}
