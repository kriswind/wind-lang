// Counting islands; see main.wind.
package main

import (
	"fmt"
	"os"
)

const maxSide = 3000

// 0 is water, 1 unvisited land, 2 land already counted.
var land []byte
var stack []int64
var top int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func visit(cell int64) {
	if land[cell] == 1 {
		land[cell] = 2
		stack[top] = cell
		top++
	}
}

func main() {
	var side int64 = 60
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		side = maxSide
	}
	n := side * side
	land = make([]byte, n)
	stack = make([]int64, n)
	for i := range land {
		if random()%100 < 45 {
			land[i] = 1
		}
	}
	var islands, largest int64
	for start := int64(0); start < n; start++ {
		if land[start] != 1 {
			continue
		}
		islands++
		var size int64
		visit(start)
		for top > 0 {
			top--
			cell := stack[top]
			size++
			row, col := cell/side, cell%side
			if row > 0 {
				visit(cell - side)
			}
			if row < side-1 {
				visit(cell + side)
			}
			if col > 0 {
				visit(cell - 1)
			}
			if col < side-1 {
				visit(cell + 1)
			}
		}
		if size > largest {
			largest = size
		}
	}
	fmt.Printf("%d islands, the largest of %d cells\n", islands, largest)
}
