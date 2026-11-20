/* Maximum subarray; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 10000000

static int64_t values[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    for (int64_t i = 0; i < n; i++) values[i] = random_number() % 2001 - 1000;
    int64_t best = values[0], best_start = 0, best_end = 0, sum = values[0], start = 0;
    for (int64_t i = 1; i < n; i++) {
        if (sum <= 0) {
            sum = values[i];
            start = i;
        } else {
            sum += values[i];
        }
        if (sum > best) {
            best = sum;
            best_start = start;
            best_end = i;
        }
    }
    printf("%lld from %lld to %lld\n", (long long)best, (long long)best_start, (long long)best_end);
    return 0;
}
