/* Merge sort inversions; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 2000000

static int64_t values[MAX], scratch[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t merge(int64_t low, int64_t middle, int64_t high) {
    int64_t i = low, j = middle, k = low, inversions = 0;
    while (i < middle && j < high) {
        if (values[j] < values[i]) {
            scratch[k++] = values[j++];
            inversions += middle - i;
        } else {
            scratch[k++] = values[i++];
        }
    }
    while (i < middle) scratch[k++] = values[i++];
    while (j < high) scratch[k++] = values[j++];
    memcpy(values + low, scratch + low, (size_t)(high - low) * sizeof *values);
    return inversions;
}

static int64_t sort(int64_t n) {
    int64_t inversions = 0;
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t low = 0; low < n - width; low += 2 * width) {
            int64_t high = low + 2 * width < n ? low + 2 * width : n;
            inversions += merge(low, low + width, high);
        }
    }
    return inversions;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    for (int64_t i = 0; i < n; i++) values[i] = random_number() % n;
    int64_t inversions = sort(n);
    int64_t checksum = 0;
    for (int64_t i = 0; i < n; i++) checksum = (checksum * 31 + values[i]) % 1000000007;
    printf("%lld inversions, checksum %lld\n", (long long)inversions, (long long)checksum);
    return 0;
}
