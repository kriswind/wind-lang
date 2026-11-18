/* Two sum; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 2000000
#define SLOTS 4194304

static int64_t values[MAX], keys[SLOTS], counts[SLOTS];
static int64_t mask;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

/* The slot holding key, or the empty slot (key -1) where it would go. */
static int64_t slot(int64_t key) {
    int64_t at = (key * 2654435761) & mask;
    while (keys[at] != -1 && keys[at] != key) at = (at + 1) & mask;
    return at;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    int64_t size = 1;
    while (size < 2 * n) size *= 2;
    mask = size - 1;
    for (int64_t i = 0; i < size; i++) keys[i] = -1;
    for (int64_t i = 0; i < n; i++) values[i] = random_number() % n;
    int64_t pairs = 0, distinct = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t value = values[i];
        int64_t want = n - value;
        int64_t at = slot(want);
        if (keys[at] == want) pairs += counts[at];
        at = slot(value);
        if (keys[at] != value) {
            keys[at] = value;
            distinct++;
        }
        counts[at]++;
    }
    printf("%lld pairs, %lld distinct values\n", (long long)pairs, (long long)distinct);
    return 0;
}
