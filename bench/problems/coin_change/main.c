/* Coin change; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_COINS 40
#define MAX_AMOUNT 1000000
#define PRIME 1000000007

static int64_t coins[MAX_COINS];
static int64_t ways[MAX_AMOUNT + 1], fewest[MAX_AMOUNT + 1];
static int64_t seed = 42;

static int64_t random_number(void) {
    seed = seed * 48271 % 2147483647;
    return seed;
}

static bool chosen(int64_t value, int64_t count) {
    for (int64_t i = 0; i < count; i++)
        if (coins[i] == value) return true;
    return false;
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t count = bench ? MAX_COINS : 8;
    int64_t limit = bench ? MAX_AMOUNT : 2000;
    for (int64_t i = 0; i < count; i++) {
        int64_t coin = 2 + random_number() % 199;
        while (chosen(coin, i)) coin = 2 + random_number() % 199;
        coins[i] = coin;
    }

    ways[0] = 1;
    for (int64_t a = 1; a <= limit; a++) ways[a] = 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t coin = coins[i];
        for (int64_t a = coin; a <= limit; a++) {
            int64_t total = ways[a] + ways[a - coin];
            if (total >= PRIME) total -= PRIME;
            ways[a] = total;
        }
    }

    fewest[0] = 0;
    for (int64_t a = 1; a <= limit; a++) fewest[a] = limit + 1;
    for (int64_t i = 0; i < count; i++) {
        int64_t coin = coins[i];
        for (int64_t a = coin; a <= limit; a++) {
            int64_t used = fewest[a - coin] + 1;
            if (used < fewest[a]) fewest[a] = used;
        }
    }

    int64_t ways_sum = 0, fewest_sum = 0, unreachable = 0;
    for (int64_t a = 1; a <= limit; a++) {
        ways_sum += ways[a];
        if (ways_sum >= PRIME) ways_sum -= PRIME;
        if (fewest[a] > limit) unreachable++;
        else fewest_sum += fewest[a];
    }
    printf("ways %lld\n", (long long)ways_sum);
    printf("fewest %lld\n", (long long)fewest_sum);
    printf("unreachable %lld\n", (long long)unreachable);
    return 0;
}
