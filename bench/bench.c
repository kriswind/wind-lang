/* The benchmark program in C; see bench.wind for what each kernel measures. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Node {
    int64_t value;
    struct Node *next;
} Node;

typedef struct {
    double x;
    double y;
    double vx;
    double vy;
} Particle;

#define SIEVE_LIMIT 20000000
#define SORT_COUNT 2000000
#define MATRIX_SIZE 400
#define LIST_COUNT 1000000
#define HASH_SIZE 2097152
#define HASH_KEYS 1000000
#define BYTES_COUNT 4000000
#define PARTICLE_COUNT 1000
#define QUEENS 12

static bool composite[SIEVE_LIMIT + 1];
static int64_t numbers[SORT_COUNT];
static double left[MATRIX_SIZE * MATRIX_SIZE];
static double right[MATRIX_SIZE * MATRIX_SIZE];
static double product[MATRIX_SIZE * MATRIX_SIZE];
static Node nodes[LIST_COUNT];
static int64_t order[LIST_COUNT];
static int64_t table[HASH_SIZE];
static unsigned char text[BYTES_COUNT];
static int64_t counts[26];
static Particle particles[PARTICLE_COUNT];
static bool columns[QUEENS];
static bool falling[2 * QUEENS];
static bool rising[2 * QUEENS];

static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t sieve(void) {
    int64_t count = 0;
    for (int64_t i = 2; i <= SIEVE_LIMIT; i++) {
        if (composite[i]) continue;
        count++;
        for (int64_t j = i * 2; j <= SIEVE_LIMIT; j += i) composite[j] = true;
    }
    return count;
}

static int64_t collatz(void) {
    int64_t best = 0;
    int64_t best_start = 0;
    for (int64_t start = 1; start < 300000; start++) {
        int64_t n = start;
        int64_t steps = 0;
        while (n != 1) {
            if (n % 2 == 0) n /= 2; else n = 3 * n + 1;
            steps++;
        }
        if (steps > best) {
            best = steps;
            best_start = start;
        }
    }
    return best_start * 1000 + best;
}

static int64_t gcd(int64_t a, int64_t b) {
    int64_t x = a;
    int64_t y = b;
    while (y != 0) {
        int64_t rest = x % y;
        x = y;
        y = rest;
    }
    return x;
}

static int64_t gcd_sum(void) {
    int64_t total = 0;
    for (int64_t i = 1; i <= 1200; i++) {
        for (int64_t j = 1; j <= 1200; j++) total += gcd(i, j);
    }
    return total;
}

static int64_t mandelbrot(void) {
    int64_t size = 600;
    int64_t total = 0;
    for (int64_t py = 0; py < size; py++) {
        for (int64_t px = 0; px < size; px++) {
            double cx = (double)px * 3.0 / (double)size - 2.0;
            double cy = (double)py * 2.0 / (double)size - 1.0;
            double x = 0.0;
            double y = 0.0;
            int64_t i = 0;
            while (i < 200 && x * x + y * y <= 4.0) {
                double next = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = next;
                i++;
            }
            total += i;
        }
    }
    return total;
}

static int64_t matmul(void) {
    int64_t n = MATRIX_SIZE;
    for (int64_t i = 0; i < n * n; i++) {
        left[i] = (double)(i % 7) + 0.5;
        right[i] = (double)(i % 5) - 1.5;
    }
    for (int64_t i = 0; i < n; i++) {
        for (int64_t j = 0; j < n; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < n; k++) sum += left[i * n + k] * right[k * n + j];
            product[i * n + j] = sum;
        }
    }
    double total = 0.0;
    for (int64_t i = 0; i < n * n; i++) total += product[i];
    return (int64_t)total;
}

static int64_t particle_sum(void) {
    for (int64_t i = 0; i < PARTICLE_COUNT; i++) {
        Particle *p = &particles[i];
        p->x = (double)(random_number() % 1000);
        p->y = (double)(random_number() % 1000);
        p->vx = (double)(random_number() % 200 - 100) / 8.0;
        p->vy = (double)(random_number() % 200 - 100) / 8.0;
    }
    for (int64_t step = 0; step < 20000; step++) {
        for (int64_t i = 0; i < PARTICLE_COUNT; i++) {
            Particle *p = &particles[i];
            p->x += p->vx * 0.5;
            p->y += p->vy * 0.5;
            if (p->x < 0.0 || p->x > 1000.0) p->vx = -p->vx;
            if (p->y < 0.0 || p->y > 1000.0) p->vy = -p->vy;
        }
    }
    double total = 0.0;
    for (int64_t i = 0; i < PARTICLE_COUNT; i++) total += particles[i].x + particles[i].y;
    return (int64_t)total;
}

static void quicksort(int64_t *values, int64_t low, int64_t high) {
    if (low >= high) return;
    int64_t pivot = values[(low + high) / 2];
    int64_t i = low;
    int64_t j = high;
    while (i <= j) {
        while (values[i] < pivot) i++;
        while (values[j] > pivot) j--;
        if (i <= j) {
            int64_t held = values[i];
            values[i] = values[j];
            values[j] = held;
            i++;
            j--;
        }
    }
    quicksort(values, low, j);
    quicksort(values, i, high);
}

static int64_t sort(void) {
    for (int64_t i = 0; i < SORT_COUNT; i++) numbers[i] = random_number() % 1000000;
    quicksort(numbers, 0, SORT_COUNT - 1);
    int64_t check = 0;
    for (int64_t i = 0; i < SORT_COUNT; i += 1000) check += numbers[i] * (i % 7 + 1);
    return check;
}

static int64_t place(int64_t row) {
    if (row == QUEENS) return 1;
    int64_t ways = 0;
    for (int64_t column = 0; column < QUEENS; column++) {
        int64_t down = row + column;
        int64_t up = row - column + QUEENS;
        if (columns[column] || falling[down] || rising[up]) continue;
        columns[column] = true;
        falling[down] = true;
        rising[up] = true;
        ways += place(row + 1);
        columns[column] = false;
        falling[down] = false;
        rising[up] = false;
    }
    return ways;
}

static int64_t hash(void) {
    for (int64_t i = 0; i < HASH_KEYS; i++) {
        int64_t key = random_number();
        int64_t slot = key % HASH_SIZE;
        while (table[slot] != 0 && table[slot] != key) slot = (slot + 1) % HASH_SIZE;
        table[slot] = key;
    }
    seed = 42;
    int64_t found = 0;
    int64_t probes = 0;
    for (int64_t i = 0; i < HASH_KEYS; i++) {
        int64_t key = random_number() + i % 2;
        int64_t slot = key % HASH_SIZE;
        while (table[slot] != 0 && table[slot] != key) {
            slot = (slot + 1) % HASH_SIZE;
            probes++;
        }
        if (table[slot] == key) found++;
    }
    return found * 100 + probes;
}

static int64_t bytes(void) {
    for (int64_t i = 0; i < BYTES_COUNT; i++) text[i] = (unsigned char)(97 + random_number() % 26);
    int64_t code = 0;
    for (int64_t i = 0; i < BYTES_COUNT; i++) {
        int64_t letter = text[i];
        counts[letter - 97]++;
        code = (code * 31 + letter) % 1000000007;
    }
    int64_t pairs = 0;
    for (int64_t i = 1; i < BYTES_COUNT; i++) {
        if (text[i - 1] == 'w' && text[i] == 'd') pairs++;
    }
    return code + counts[4] + pairs * 7;
}

static int64_t list(void) {
    for (int64_t i = 0; i < LIST_COUNT; i++) {
        order[i] = i;
        nodes[i].value = i % 100;
    }
    for (int64_t i = LIST_COUNT - 1; i > 0; i--) {
        int64_t j = random_number() % (i + 1);
        int64_t held = order[i];
        order[i] = order[j];
        order[j] = held;
    }
    for (int64_t i = 0; i < LIST_COUNT - 1; i++) nodes[order[i]].next = &nodes[order[i + 1]];
    nodes[order[LIST_COUNT - 1]].next = NULL;

    int64_t total = 0;
    for (int64_t pass = 0; pass < 20; pass++) {
        for (Node *at = &nodes[order[0]]; at != NULL; at = at->next) total += at->value + pass;
    }
    return total;
}

int main(int argc, char **argv) {
    const char *kernel = argc > 1 ? argv[1] : "";
    int64_t result;
    if (!strcmp(kernel, "fib")) result = fib(38);
    else if (!strcmp(kernel, "sieve")) result = sieve();
    else if (!strcmp(kernel, "collatz")) result = collatz();
    else if (!strcmp(kernel, "gcd")) result = gcd_sum();
    else if (!strcmp(kernel, "mandelbrot")) result = mandelbrot();
    else if (!strcmp(kernel, "matmul")) result = matmul();
    else if (!strcmp(kernel, "particles")) result = particle_sum();
    else if (!strcmp(kernel, "sort")) result = sort();
    else if (!strcmp(kernel, "queens")) result = place(0);
    else if (!strcmp(kernel, "hash")) result = hash();
    else if (!strcmp(kernel, "bytes")) result = bytes();
    else if (!strcmp(kernel, "list")) result = list();
    else {
        printf("unknown kernel '%s'\n", kernel);
        return 2;
    }
    printf("%lld\n", (long long)result);
    return 0;
}
