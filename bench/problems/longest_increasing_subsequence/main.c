/* Longest increasing subsequence; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 3000000

static int64_t values[MAX], tails[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t longest(int64_t n) {
    int64_t length = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t x = values[i];
        int64_t low = 0, high = length;
        while (low < high) {
            int64_t middle = (low + high) / 2;
            if (tails[middle] < x) low = middle + 1;
            else high = middle;
        }
        tails[low] = x;
        if (low == length) length++;
    }
    return length;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 5000;
    for (int64_t i = 0; i < n; i++) values[i] = random_number() % 1000000;
    int64_t length = longest(n);
    int64_t sum = 0;
    for (int64_t k = 0; k < length; k++) sum += tails[k];
    printf("length %lld\n", (long long)length);
    printf("tails %lld\n", (long long)sum);
    return 0;
}
