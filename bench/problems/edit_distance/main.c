/* Edit distance; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 6000

static char a[MAX], b[MAX];
static int64_t previous[MAX + 1], current[MAX + 1];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t distance(int64_t n) {
    for (int64_t j = 0; j <= n; j++) previous[j] = j;
    for (int64_t i = 1; i <= n; i++) {
        current[0] = i;
        for (int64_t j = 1; j <= n; j++) {
            int64_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int64_t best = previous[j - 1] + cost;
            if (previous[j] + 1 < best) best = previous[j] + 1;
            if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
            current[j] = best;
        }
        memcpy(previous, current, (size_t)(n + 1) * sizeof *previous);
    }
    return previous[n];
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 300;
    for (int64_t i = 0; i < n; i++) a[i] = (char)(97 + random_number() % 4);
    for (int64_t i = 0; i < n; i++) b[i] = (char)(97 + random_number() % 4);
    printf("%lld\n", (long long)distance(n));
    return 0;
}
