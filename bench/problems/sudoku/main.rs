// Sudoku; see main.wind.
use std::env;

const PUZZLES: [&str; 8] = [
    "8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..",
    "4.....8.5.3..........7......2.....6.....8.4......1.......6.3.7.5..2.....1.4......",
    "52...6.........7.13...........4..8..6......5...........418.........3..2...87.....",
    "6.....8.3.4.7.................5.4.7.3..2.....1.6.......2.....5.....8.6......1....",
    "48.3............71.2.......7.5....6....2..8.............1.76...3.....4......5....",
    "....14....3....2...7..........9...3.6.1.............8.2.....1.4....5.6.....7.8...",
    "1.......2.9.4...5...6...7...5.9.3.......7.......85..4.7.....6...3...9.8...2.....1",
    "1....7.9..3..2...8..96..5....53..9...1..8...26....4...3......1..4......7..7...3..",
];

struct Board {
    bits: [i64; 512],
    grid: [i64; 81],
    rows: [i64; 9],
    cols: [i64; 9],
    boxes: [i64; 9],
    guesses: i64,
}

fn box_of(cell: usize) -> usize {
    cell / 27 * 3 + cell % 9 / 3
}

impl Board {
    // Puts digit in cell, or takes it out again.
    fn place(&mut self, cell: usize, digit: i64) {
        let bit = 1i64 << (digit - 1);
        self.grid[cell] = digit;
        self.rows[cell / 9] ^= bit;
        self.cols[cell % 9] ^= bit;
        self.boxes[box_of(cell)] ^= bit;
    }

    fn solve(&mut self) -> bool {
        let mut best: i64 = -1;
        let mut fewest = 10;
        for cell in 0..81 {
            if self.grid[cell] == 0 {
                let used = self.rows[cell / 9] | self.cols[cell % 9] | self.boxes[box_of(cell)];
                let free = 9 - self.bits[used as usize];
                if free < fewest {
                    fewest = free;
                    best = cell as i64;
                }
            }
        }
        if best == -1 {
            return true;
        }
        let best = best as usize;
        let used = self.rows[best / 9] | self.cols[best % 9] | self.boxes[box_of(best)];
        for digit in 1..=9 {
            if used & (1i64 << (digit - 1)) == 0 {
                self.guesses += 1;
                self.place(best, digit);
                if self.solve() {
                    return true;
                }
                self.place(best, digit);
                self.grid[best] = 0;
            }
        }
        false
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let count = if bench { PUZZLES.len() } else { 3 };
    let rounds = if bench { 10 } else { 1 };
    let mut board = Board { bits: [0; 512], grid: [0; 81], rows: [0; 9], cols: [0; 9], boxes: [0; 9], guesses: 0 };
    for i in 1..512 {
        board.bits[i] = board.bits[i >> 1] + (i as i64 & 1);
    }
    let mut solved = 0;
    let mut checksum: i64 = 0;
    for _ in 0..rounds {
        for p in 0..count {
            board.rows = [0; 9];
            board.cols = [0; 9];
            board.boxes = [0; 9];
            let puzzle = PUZZLES[p].as_bytes();
            for cell in 0..81 {
                board.grid[cell] = 0;
                if puzzle[cell] != b'.' {
                    board.place(cell, puzzle[cell] as i64 - 48);
                }
            }
            if board.solve() {
                solved += 1;
                let mut first = 0;
                for i in 0..9 {
                    first = first * 10 + board.grid[i];
                }
                checksum += first;
            }
        }
    }
    println!("{} solved, checksum {}, {} guesses", solved, checksum, board.guesses);
}
