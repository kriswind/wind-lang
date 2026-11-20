/* Game of Life; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_SIZE 256

static uint8_t first[MAX_SIZE * MAX_SIZE], second[MAX_SIZE * MAX_SIZE];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void step(const uint8_t *source, uint8_t *target, int64_t size) {
    for (int64_t row = 0; row < size; row++) {
        int64_t up = (row + size - 1) % size * size;
        int64_t middle = row * size;
        int64_t down = (row + 1) % size * size;
        for (int64_t column = 0; column < size; column++) {
            int64_t left = (column + size - 1) % size;
            int64_t right = (column + 1) % size;
            uint8_t count = source[up + left] + source[up + column] + source[up + right]
                + source[middle + left] + source[middle + right]
                + source[down + left] + source[down + column] + source[down + right];
            bool alive = source[middle + column] == 1;
            target[middle + column] = count == 3 || (alive && count == 2) ? 1 : 0;
        }
    }
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t size = bench ? MAX_SIZE : 32;
    int64_t generations = bench ? 500 : 40;
    int64_t cells = size * size;
    for (int64_t i = 0; i < cells; i++) first[i] = random_number() % 3 == 0 ? 1 : 0;
    for (int64_t g = 0; g < generations; g += 2) {
        step(first, second, size);
        step(second, first, size);
    }
    int64_t alive = 0, checksum = 0;
    for (int64_t i = 0; i < cells; i++) {
        alive += first[i];
        checksum = (checksum * 31 + first[i]) % 1000000007;
    }
    printf("%lld alive, checksum %lld\n", (long long)alive, (long long)checksum);
    return 0;
}
