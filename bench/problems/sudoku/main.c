/* Sudoku; see main.wind. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *PUZZLES[] = {
    "8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..",
    "4.....8.5.3..........7......2.....6.....8.4......1.......6.3.7.5..2.....1.4......",
    "52...6.........7.13...........4..8..6......5...........418.........3..2...87.....",
    "6.....8.3.4.7.................5.4.7.3..2.....1.6.......2.....5.....8.6......1....",
    "48.3............71.2.......7.5....6....2..8.............1.76...3.....4......5....",
    "....14....3....2...7..........9...3.6.1.............8.2.....1.4....5.6.....7.8...",
    "1.......2.9.4...5...6...7...5.9.3.......7.......85..4.7.....6...3...9.8...2.....1",
    "1....7.9..3..2...8..96..5....53..9...1..8...26....4...3......1..4......7..7...3..",
};

static int64_t bits[512], grid[81], rows[9], cols[9], boxes[9];
static int64_t guesses = 0;

static int64_t box(int64_t cell) {
    return cell / 27 * 3 + cell % 9 / 3;
}

/* Puts digit in cell, or takes it out again. */
static void place(int64_t cell, int64_t digit) {
    int64_t bit = INT64_C(1) << (digit - 1);
    grid[cell] = digit;
    rows[cell / 9] ^= bit;
    cols[cell % 9] ^= bit;
    boxes[box(cell)] ^= bit;
}

static bool solve(void) {
    int64_t best = -1, fewest = 10;
    for (int64_t cell = 0; cell < 81; cell++) {
        if (grid[cell] == 0) {
            int64_t free = 9 - bits[rows[cell / 9] | cols[cell % 9] | boxes[box(cell)]];
            if (free < fewest) {
                fewest = free;
                best = cell;
            }
        }
    }
    if (best == -1) return true;
    int64_t used = rows[best / 9] | cols[best % 9] | boxes[box(best)];
    for (int64_t digit = 1; digit <= 9; digit++) {
        if ((used & (INT64_C(1) << (digit - 1))) == 0) {
            guesses++;
            place(best, digit);
            if (solve()) return true;
            place(best, digit);
            grid[best] = 0;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    int64_t count = bench ? (int64_t)(sizeof PUZZLES / sizeof PUZZLES[0]) : 3;
    int64_t rounds = bench ? 10 : 1;
    for (int64_t i = 1; i < 512; i++) bits[i] = bits[i >> 1] + (i & 1);
    int64_t solved = 0, checksum = 0;
    for (int64_t round = 0; round < rounds; round++) {
        for (int64_t p = 0; p < count; p++) {
            for (int64_t i = 0; i < 9; i++) rows[i] = cols[i] = boxes[i] = 0;
            for (int64_t cell = 0; cell < 81; cell++) {
                grid[cell] = 0;
                char ch = PUZZLES[p][cell];
                if (ch != '.') place(cell, ch - 48);
            }
            if (solve()) {
                solved++;
                int64_t first = 0;
                for (int64_t i = 0; i < 9; i++) first = first * 10 + grid[i];
                checksum += first;
            }
        }
    }
    printf("%lld solved, checksum %lld, %lld guesses\n", (long long)solved, (long long)checksum,
           (long long)guesses);
    return 0;
}
