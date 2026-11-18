/* Unique paths with obstacles; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 5000
#define PRIME 1000000007

static int64_t paths[MAX];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

int main(int argc, char **argv) {
    int64_t size = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 120;
    int64_t rows = size, columns = size, obstacles = 0;
    for (int64_t j = 0; j < columns; j++) paths[j] = 0;
    for (int64_t i = 0; i < rows; i++) {
        for (int64_t j = 0; j < columns; j++) {
            bool corner = (i == 0 && j == 0) || (i == rows - 1 && j == columns - 1);
            bool blocked = random_number() % 10 == 0 && !corner;
            if (blocked) {
                obstacles++;
                paths[j] = 0;
            } else if (i == 0 && j == 0) {
                paths[j] = 1;
            } else if (j > 0) {
                int64_t total = paths[j] + paths[j - 1];
                if (total >= PRIME) total -= PRIME;
                paths[j] = total;
            }
        }
    }
    printf("paths %lld\n", (long long)paths[columns - 1]);
    printf("obstacles %lld\n", (long long)obstacles);
    return 0;
}
