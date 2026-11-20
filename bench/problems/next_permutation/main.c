/* Next permutation; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 10

static int64_t items[MAX], weights[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static bool next_permutation(int64_t n) {
    int64_t i = n - 2;
    while (i >= 0 && items[i] >= items[i + 1]) i--;
    if (i < 0) return false;
    int64_t j = n - 1;
    while (items[j] <= items[i]) j--;
    int64_t held = items[i];
    items[i] = items[j];
    items[j] = held;
    for (int64_t low = i + 1, high = n - 1; low < high; low++, high--) {
        held = items[low];
        items[low] = items[high];
        items[high] = held;
    }
    return true;
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t n = bench ? MAX : 7;
    int64_t rounds = bench ? 4 : 2;
    int64_t count = 0, total = 0, checksum = 0;
    for (int64_t round = 0; round < rounds; round++) {
        for (int64_t i = 0; i < n; i++) {
            items[i] = i;
            weights[i] = random_number() % 1000 + 1;
        }
        do {
            int64_t value = 0;
            for (int64_t i = 0; i < n; i++) value += items[i] * weights[i];
            count++;
            total += value;
            checksum = (checksum * 31 + value) % 1000000007;
        } while (next_permutation(n));
    }
    printf("%lld permutations, total %lld, checksum %lld\n", (long long)count, (long long)total,
           (long long)checksum);
    return 0;
}
