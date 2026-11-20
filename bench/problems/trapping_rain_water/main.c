/* Trapping rain water; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 10000000

static int64_t heights[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t trapped(int64_t n) {
    int64_t left = 0, right = n - 1, left_max = 0, right_max = 0, water = 0;
    while (left < right) {
        if (heights[left] < heights[right]) {
            if (heights[left] >= left_max) left_max = heights[left];
            else water += left_max - heights[left];
            left++;
        } else {
            if (heights[right] >= right_max) right_max = heights[right];
            else water += right_max - heights[right];
            right--;
        }
    }
    return water;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    for (int64_t i = 0; i < n; i++) heights[i] = random_number() % 1000;
    printf("%lld\n", (long long)trapped(n));
    return 0;
}
