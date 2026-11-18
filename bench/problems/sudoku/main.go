// Sudoku; see main.wind.
package main

import (
	"fmt"
	"os"
)

var puzzles = []string{
	"8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..",
	"4.....8.5.3..........7......2.....6.....8.4......1.......6.3.7.5..2.....1.4......",
	"52...6.........7.13...........4..8..6......5...........418.........3..2...87.....",
	"6.....8.3.4.7.................5.4.7.3..2.....1.6.......2.....5.....8.6......1....",
	"48.3............71.2.......7.5....6....2..8.............1.76...3.....4......5....",
	"....14....3....2...7..........9...3.6.1.............8.2.....1.4....5.6.....7.8...",
	"1.......2.9.4...5...6...7...5.9.3.......7.......85..4.7.....6...3...9.8...2.....1",
	"1....7.9..3..2...8..96..5....53..9...1..8...26....4...3......1..4......7..7...3..",
}

var bits [512]int64
var grid [81]int64
var rows, cols, boxes [9]int64
var guesses int64

func box(cell int64) int64 {
	return cell/27*3 + cell%9/3
}

// Puts digit in cell, or takes it out again.
func place(cell, digit int64) {
	bit := int64(1) << (digit - 1)
	grid[cell] = digit
	rows[cell/9] ^= bit
	cols[cell%9] ^= bit
	boxes[box(cell)] ^= bit
}

func solve() bool {
	var best, fewest int64 = -1, 10
	for cell := int64(0); cell < 81; cell++ {
		if grid[cell] == 0 {
			free := 9 - bits[rows[cell/9]|cols[cell%9]|boxes[box(cell)]]
			if free < fewest {
				fewest = free
				best = cell
			}
		}
	}
	if best == -1 {
		return true
	}
	used := rows[best/9] | cols[best%9] | boxes[box(best)]
	for digit := int64(1); digit <= 9; digit++ {
		if used&(int64(1)<<(digit-1)) == 0 {
			guesses++
			place(best, digit)
			if solve() {
				return true
			}
			place(best, digit)
			grid[best] = 0
		}
	}
	return false
}

func main() {
	bench := len(os.Args) > 1 && os.Args[1] == "bench"
	count, rounds := 3, 1
	if bench {
		count, rounds = len(puzzles), 10
	}
	for i := 1; i < 512; i++ {
		bits[i] = bits[i>>1] + int64(i&1)
	}
	var solved, checksum int64
	for round := 0; round < rounds; round++ {
		for p := 0; p < count; p++ {
			for i := 0; i < 9; i++ {
				rows[i], cols[i], boxes[i] = 0, 0, 0
			}
			for cell := int64(0); cell < 81; cell++ {
				grid[cell] = 0
				ch := puzzles[p][cell]
				if ch != '.' {
					place(cell, int64(ch)-48)
				}
			}
			if solve() {
				solved++
				var first int64
				for i := 0; i < 9; i++ {
					first = first*10 + grid[i]
				}
				checksum += first
			}
		}
	}
	fmt.Printf("%d solved, checksum %d, %d guesses\n", solved, checksum, guesses)
}
