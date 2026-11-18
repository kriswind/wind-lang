// Breadth-first search through a maze; see main.wind.
package main

import (
	"fmt"
	"os"
)

const maxSide = 2000

var wall []byte
var dist, queue []int64
var tail int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func visit(cell, d int64) {
	if wall[cell] == 0 && dist[cell] == -1 {
		dist[cell] = d
		queue[tail] = cell
		tail++
	}
}

func main() {
	var side int64 = 100
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		side = maxSide
	}
	n := side * side
	wall = make([]byte, n)
	dist = make([]int64, n)
	queue = make([]int64, n)
	for i := range wall {
		if random()%100 < 30 {
			wall[i] = 1
		}
		dist[i] = -1
	}
	for _, cell := range []int64{0, 1, side, n - 1, n - 2, n - 1 - side} {
		wall[cell] = 0
	}
	var head int64
	dist[0] = 0
	queue[tail] = 0
	tail++
	for head < tail {
		cell := queue[head]
		head++
		d := dist[cell] + 1
		row, col := cell/side, cell%side
		if row > 0 {
			visit(cell-side, d)
		}
		if row < side-1 {
			visit(cell+side, d)
		}
		if col > 0 {
			visit(cell-1, d)
		}
		if col < side-1 {
			visit(cell+1, d)
		}
	}
	fmt.Printf("%d steps, %d cells reached\n", dist[n-1], tail)
}
