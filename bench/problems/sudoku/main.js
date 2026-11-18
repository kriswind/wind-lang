// Sudoku; see main.wind.
const PUZZLES = [
    "8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..",
    "4.....8.5.3..........7......2.....6.....8.4......1.......6.3.7.5..2.....1.4......",
    "52...6.........7.13...........4..8..6......5...........418.........3..2...87.....",
    "6.....8.3.4.7.................5.4.7.3..2.....1.6.......2.....5.....8.6......1....",
    "48.3............71.2.......7.5....6....2..8.............1.76...3.....4......5....",
    "....14....3....2...7..........9...3.6.1.............8.2.....1.4....5.6.....7.8...",
    "1.......2.9.4...5...6...7...5.9.3.......7.......85..4.7.....6...3...9.8...2.....1",
    "1....7.9..3..2...8..96..5....53..9...1..8...26....4...3......1..4......7..7...3..",
];
const bench = process.argv[2] === "bench";
const count = bench ? PUZZLES.length : 3;
const rounds = bench ? 10 : 1;

const bits = new Int32Array(512);
for (let i = 1; i < 512; i++) bits[i] = bits[i >> 1] + (i & 1);
const grid = new Int32Array(81);
const rows = new Int32Array(9);
const cols = new Int32Array(9);
const boxes = new Int32Array(9);
let guesses = 0;

function box(cell) {
    return Math.floor(cell / 27) * 3 + Math.floor((cell % 9) / 3);
}

// Puts digit in cell, or takes it out again.
function place(cell, digit) {
    const bit = 1 << (digit - 1);
    grid[cell] = digit;
    rows[Math.floor(cell / 9)] ^= bit;
    cols[cell % 9] ^= bit;
    boxes[box(cell)] ^= bit;
}

function solve() {
    let best = -1;
    let fewest = 10;
    for (let cell = 0; cell < 81; cell++) {
        if (grid[cell] === 0) {
            const free = 9 - bits[rows[Math.floor(cell / 9)] | cols[cell % 9] | boxes[box(cell)]];
            if (free < fewest) {
                fewest = free;
                best = cell;
            }
        }
    }
    if (best === -1) return true;
    const used = rows[Math.floor(best / 9)] | cols[best % 9] | boxes[box(best)];
    for (let digit = 1; digit <= 9; digit++) {
        if ((used & (1 << (digit - 1))) === 0) {
            guesses++;
            place(best, digit);
            if (solve()) return true;
            place(best, digit);
            grid[best] = 0;
        }
    }
    return false;
}

let solved = 0;
let checksum = 0;
for (let round = 0; round < rounds; round++) {
    for (let p = 0; p < count; p++) {
        rows.fill(0);
        cols.fill(0);
        boxes.fill(0);
        for (let cell = 0; cell < 81; cell++) {
            grid[cell] = 0;
            const ch = PUZZLES[p].charCodeAt(cell);
            if (ch !== 46) place(cell, ch - 48);
        }
        if (solve()) {
            solved++;
            let first = 0;
            for (let i = 0; i < 9; i++) first = first * 10 + grid[i];
            checksum += first;
        }
    }
}
console.log(solved + " solved, checksum " + checksum + ", " + guesses + " guesses");
