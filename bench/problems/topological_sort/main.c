/* Topological sort; see main.wind. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX 1000000
#define MAX_EDGES (4 * MAX)

static int64_t label[MAX], waiting[MAX], first[MAX + 1], fill[MAX], heap[MAX];
static int64_t source[MAX_EDGES], target[MAX_EDGES], after[MAX_EDGES];
static int64_t size = 0;
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static void push(int64_t node) {
    int64_t at = size++;
    while (at > 0) {
        int64_t parent = (at - 1) / 2;
        if (heap[parent] <= node) break;
        heap[at] = heap[parent];
        at = parent;
    }
    heap[at] = node;
}

static int64_t pop(void) {
    int64_t top = heap[0];
    int64_t last = heap[--size];
    int64_t at = 0;
    for (;;) {
        int64_t child = 2 * at + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1] < heap[child]) child++;
        if (last <= heap[child]) break;
        heap[at] = heap[child];
        at = child;
    }
    heap[at] = last;
    return top;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 && strcmp(argv[1], "bench") == 0 ? MAX : 1000;
    int64_t m = 4 * n;
    for (int64_t i = 0; i < n; i++) label[i] = i;
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = random_number() % (i + 1);
        int64_t held = label[i];
        label[i] = label[j];
        label[j] = held;
    }
    int64_t edges = 0;
    for (int64_t e = 0; e < m; e++) {
        int64_t a = random_number() % n;
        int64_t b = random_number() % n;
        if (a == b) continue;
        if (a > b) {
            int64_t held = a;
            a = b;
            b = held;
        }
        source[edges] = label[a];
        target[edges] = label[b];
        edges++;
    }
    for (int64_t e = 0; e < edges; e++) {
        first[source[e] + 1]++;
        waiting[target[e]]++;
    }
    for (int64_t i = 0; i < n; i++) first[i + 1] += first[i];
    for (int64_t i = 0; i < n; i++) fill[i] = first[i];
    for (int64_t e = 0; e < edges; e++) after[fill[source[e]]++] = target[e];

    for (int64_t i = 0; i < n; i++) {
        if (waiting[i] == 0) push(i);
    }
    int64_t placed = 0, checksum = 0;
    while (size > 0) {
        int64_t node = pop();
        placed++;
        checksum = (checksum * 31 + node) % 1000000007;
        for (int64_t k = first[node]; k < first[node + 1]; k++) {
            int64_t v = after[k];
            if (--waiting[v] == 0) push(v);
        }
    }
    printf("%lld edges, %lld nodes placed, checksum %lld\n", (long long)edges, (long long)placed,
           (long long)checksum);
    return 0;
}
