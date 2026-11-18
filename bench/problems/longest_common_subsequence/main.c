/* Longest common subsequence; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 8000

static char a[MAX], b[MAX];
static int64_t row_one[MAX + 1], row_two[MAX + 1];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t common(int64_t n, int64_t m) {
    int64_t *previous = row_one, *current = row_two;
    for (int64_t j = 0; j <= m; j++) previous[j] = 0;
    for (int64_t i = 1; i <= n; i++) {
        current[0] = 0;
        char letter = a[i - 1];
        for (int64_t j = 1; j <= m; j++) {
            if (letter == b[j - 1]) current[j] = previous[j - 1] + 1;
            else current[j] = previous[j] > current[j - 1] ? previous[j] : current[j - 1];
        }
        int64_t *held = previous;
        previous = current;
        current = held;
    }
    return previous[m];
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 400;
    int64_t m = n * 3 / 4;
    for (int64_t i = 0; i < n; i++) a[i] = (char)(97 + random_number() % 8);
    for (int64_t j = 0; j < m; j++) b[j] = (char)(97 + random_number() % 8);
    printf("%lld\n", (long long)common(n, m));
    return 0;
}
