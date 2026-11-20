/* Matrix Fibonacci; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PRIME 67108859

typedef struct {
    int64_t a, b, c, d;
} Matrix;

static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static Matrix multiply(Matrix x, Matrix y) {
    Matrix m = {
        (x.a * y.a + x.b * y.c) % PRIME, (x.a * y.b + x.b * y.d) % PRIME,
        (x.c * y.a + x.d * y.c) % PRIME, (x.c * y.b + x.d * y.d) % PRIME,
    };
    return m;
}

static int64_t fibonacci(int64_t n) {
    Matrix result = {1, 0, 0, 1};
    Matrix base = {1, 1, 1, 0};
    int64_t left = n;
    while (left > 0) {
        if ((left & 1) == 1) result = multiply(result, base);
        base = multiply(base, base);
        left >>= 1;
    }
    return result.b;
}

int main(int argc, char **argv) {
    int64_t queries = argc > 1 && strcmp(argv[1], "bench") == 0 ? 1000000 : 1000;
    int64_t total = 0;
    for (int64_t i = 0; i < queries; i++) total = (total + fibonacci(random_number())) % PRIME;
    printf("%lld\n", (long long)total);
    return 0;
}
