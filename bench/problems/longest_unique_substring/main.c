/* Longest unique substring; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 20000000

static char text[MAX];
static int64_t last[26];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 10000;
    for (int64_t i = 0; i < n; i++) text[i] = (char)(97 + random_number() % 26);
    for (int64_t c = 0; c < 26; c++) last[c] = -1;
    int64_t start = 0, best = 0, best_start = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = text[i] - 97;
        if (last[c] >= start) start = last[c] + 1;
        last[c] = i;
        if (i - start + 1 > best) {
            best = i - start + 1;
            best_start = start;
        }
    }
    printf("%lld letters from %lld: %.*s\n", (long long)best, (long long)best_start,
           (int)best, text + best_start);
    return 0;
}
