# Sudoku; see main.wind. This version is the reference that the expected
# answers come from.
import sys

PUZZLES = [
    "8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..",
    "4.....8.5.3..........7......2.....6.....8.4......1.......6.3.7.5..2.....1.4......",
    "52...6.........7.13...........4..8..6......5...........418.........3..2...87.....",
    "6.....8.3.4.7.................5.4.7.3..2.....1.6.......2.....5.....8.6......1....",
    "48.3............71.2.......7.5....6....2..8.............1.76...3.....4......5....",
    "....14....3....2...7..........9...3.6.1.............8.2.....1.4....5.6.....7.8...",
    "1.......2.9.4...5...6...7...5.9.3.......7.......85..4.7.....6...3...9.8...2.....1",
    "1....7.9..3..2...8..96..5....53..9...1..8...26....4...3......1..4......7..7...3..",
]

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
count = len(PUZZLES) if bench else 3
rounds = 10 if bench else 1

bits = [0] * 512
for i in range(1, 512):
    bits[i] = bits[i >> 1] + (i & 1)

grid = [0] * 81
rows = [0] * 9
cols = [0] * 9
boxes = [0] * 9
guesses = 0


def box(cell):
    return cell // 27 * 3 + cell % 9 // 3


def place(cell, digit):
    bit = 1 << (digit - 1)
    grid[cell] = digit
    rows[cell // 9] ^= bit
    cols[cell % 9] ^= bit
    boxes[box(cell)] ^= bit


def solve():
    global guesses
    best = -1
    fewest = 10
    for cell in range(81):
        if grid[cell] == 0:
            free = 9 - bits[rows[cell // 9] | cols[cell % 9] | boxes[box(cell)]]
            if free < fewest:
                fewest = free
                best = cell
    if best == -1:
        return True
    used = rows[best // 9] | cols[best % 9] | boxes[box(best)]
    for digit in range(1, 10):
        if used & (1 << (digit - 1)) == 0:
            guesses += 1
            place(best, digit)
            if solve():
                return True
            place(best, digit)
            grid[best] = 0
    return False


solved = 0
checksum = 0
for _ in range(rounds):
    for p in range(count):
        for i in range(9):
            rows[i] = cols[i] = boxes[i] = 0
        for cell in range(81):
            grid[cell] = 0
            ch = PUZZLES[p][cell]
            if ch != ".":
                place(cell, ord(ch) - 48)
        if solve():
            solved += 1
            first = 0
            for i in range(9):
                first = first * 10 + grid[i]
            checksum += first
print("%d solved, checksum %d, %d guesses" % (solved, checksum, guesses))
