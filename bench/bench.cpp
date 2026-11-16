// The benchmark program in C++; see bench.wind for what each kernel measures.
// The matrix product keeps its matrices in fixed arrays, which the compiler
// does more with than it can with vectors, and the sort works through a
// plain pointer.

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

struct Node {
    int64_t value = 0;
    Node *next = nullptr;
};

struct Particle {
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
};

constexpr int64_t SIEVE_LIMIT = 20000000;
constexpr int64_t SORT_COUNT = 2000000;
constexpr int64_t MATRIX_SIZE = 400;
constexpr int64_t LIST_COUNT = 1000000;
constexpr int64_t HASH_SIZE = 2097152;
constexpr int64_t HASH_KEYS = 1000000;
constexpr int64_t BYTES_COUNT = 4000000;
constexpr int64_t PARTICLE_COUNT = 1000;
constexpr int64_t QUEENS = 12;

// Each kernel sizes the vectors it uses.
static std::vector<char> composite;
static std::vector<int64_t> numbers;
static double left[MATRIX_SIZE * MATRIX_SIZE];
static double right[MATRIX_SIZE * MATRIX_SIZE];
static double product[MATRIX_SIZE * MATRIX_SIZE];
static std::vector<Node> nodes;
static std::vector<int64_t> order;
static std::vector<int64_t> table;
static std::vector<unsigned char> text;
static int64_t counts[26];
static std::vector<Particle> particles;
static bool columns[QUEENS];
static bool falling[2 * QUEENS];
static bool rising[2 * QUEENS];

static int64_t seed = 42;

static int64_t random_number() {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t sieve() {
    composite.assign(SIEVE_LIMIT + 1, 0);
    int64_t count = 0;
    for (int64_t i = 2; i <= SIEVE_LIMIT; i++) {
        if (composite[i]) continue;
        count++;
        for (int64_t j = i * 2; j <= SIEVE_LIMIT; j += i) composite[j] = 1;
    }
    return count;
}

static int64_t collatz() {
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

static int64_t gcd_sum() {
    int64_t total = 0;
    for (int64_t i = 1; i <= 1200; i++) {
        for (int64_t j = 1; j <= 1200; j++) total += gcd(i, j);
    }
    return total;
}

static int64_t mandelbrot() {
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

static int64_t matmul() {
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

static int64_t particle_sum() {
    particles.assign(PARTICLE_COUNT, Particle());
    Particle *all = particles.data();
    for (int64_t i = 0; i < PARTICLE_COUNT; i++) {
        Particle *p = &all[i];
        p->x = (double)(random_number() % 1000);
        p->y = (double)(random_number() % 1000);
        p->vx = (double)(random_number() % 200 - 100) / 8.0;
        p->vy = (double)(random_number() % 200 - 100) / 8.0;
    }
    for (int64_t step = 0; step < 20000; step++) {
        for (int64_t i = 0; i < PARTICLE_COUNT; i++) {
            Particle *p = &all[i];
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
            std::swap(values[i], values[j]);
            i++;
            j--;
        }
    }
    quicksort(values, low, j);
    quicksort(values, i, high);
}

static int64_t sort() {
    numbers.assign(SORT_COUNT, 0);
    for (int64_t i = 0; i < SORT_COUNT; i++) numbers[i] = random_number() % 1000000;
    quicksort(numbers.data(), 0, SORT_COUNT - 1);
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

static int64_t hash() {
    table.assign(HASH_SIZE, 0);
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

static int64_t bytes() {
    text.assign(BYTES_COUNT, 0);
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

static int64_t list() {
    nodes.assign(LIST_COUNT, Node());
    order.assign(LIST_COUNT, 0);
    for (int64_t i = 0; i < LIST_COUNT; i++) {
        order[i] = i;
        nodes[i].value = i % 100;
    }
    for (int64_t i = LIST_COUNT - 1; i > 0; i--) {
        int64_t j = random_number() % (i + 1);
        std::swap(order[i], order[j]);
    }
    for (int64_t i = 0; i < LIST_COUNT - 1; i++) nodes[order[i]].next = &nodes[order[i + 1]];
    nodes[order[LIST_COUNT - 1]].next = nullptr;

    int64_t total = 0;
    for (int64_t pass = 0; pass < 20; pass++) {
        for (Node *at = &nodes[order[0]]; at != nullptr; at = at->next) total += at->value + pass;
    }
    return total;
}

int main(int argc, char **argv) {
    std::string kernel = argc > 1 ? argv[1] : "";
    int64_t result;
    if (kernel == "fib") result = fib(38);
    else if (kernel == "sieve") result = sieve();
    else if (kernel == "collatz") result = collatz();
    else if (kernel == "gcd") result = gcd_sum();
    else if (kernel == "mandelbrot") result = mandelbrot();
    else if (kernel == "matmul") result = matmul();
    else if (kernel == "particles") result = particle_sum();
    else if (kernel == "sort") result = sort();
    else if (kernel == "queens") result = place(0);
    else if (kernel == "hash") result = hash();
    else if (kernel == "bytes") result = bytes();
    else if (kernel == "list") result = list();
    else {
        std::cout << "unknown kernel '" << kernel << "'\n";
        return 2;
    }
    std::cout << result << "\n";
    return 0;
}
