// Game of Life; see main.wind.
package main

import (
	"fmt"
	"os"
)

var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func step(source, target []uint8, size int) {
	for row := 0; row < size; row++ {
		up := (row + size - 1) % size * size
		middle := row * size
		down := (row + 1) % size * size
		for column := 0; column < size; column++ {
			left := (column + size - 1) % size
			right := (column + 1) % size
			count := source[up+left] + source[up+column] + source[up+right] +
				source[middle+left] + source[middle+right] +
				source[down+left] + source[down+column] + source[down+right]
			alive := source[middle+column] == 1
			if count == 3 || (alive && count == 2) {
				target[middle+column] = 1
			} else {
				target[middle+column] = 0
			}
		}
	}
}

func main() {
	bench := len(os.Args) > 1 && os.Args[1] == "bench"
	size, generations := 32, 40
	if bench {
		size, generations = 256, 500
	}
	cells := size * size
	first := make([]uint8, cells)
	second := make([]uint8, cells)
	for i := range first {
		if random()%3 == 0 {
			first[i] = 1
		}
	}
	for g := 0; g < generations; g += 2 {
		step(first, second, size)
		step(second, first, size)
	}
	var alive, checksum int64
	for _, cell := range first {
		alive += int64(cell)
		checksum = (checksum*31 + int64(cell)) % 1000000007
	}
	fmt.Printf("%d alive, checksum %d\n", alive, checksum)
}
