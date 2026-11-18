/* Sliding window maximum; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 10000000

static int64_t values[MAX], deque[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t n = bench ? MAX : 1000;
    int64_t k = bench ? 1000 : 16;
    for (int64_t i = 0; i < n; i++) values[i] = random_number() % 1000000;
    int64_t head = 0, tail = 0, sum = 0;
    for (int64_t i = 0; i < n; i++) {
        while (tail > head && values[deque[tail - 1]] <= values[i]) tail--;
        deque[tail++] = i;
        if (deque[head] <= i - k) head++;
        if (i >= k - 1) sum += values[deque[head]];
    }
    printf("%lld windows of %lld, the maxima summing to %lld\n", (long long)(n - k + 1),
           (long long)k, (long long)sum);
    return 0;
}
