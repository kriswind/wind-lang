// Dijkstra on a grid; see main.wind.
package main

import (
	"fmt"
	"os"
)

const maxSide = 1500

var cost, dist, heap []int64
var size, n int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func push(key int64) {
	at := size
	size++
	for at > 0 {
		parent := (at - 1) / 2
		if heap[parent] <= key {
			break
		}
		heap[at] = heap[parent]
		at = parent
	}
	heap[at] = key
}

func pop() int64 {
	top := heap[0]
	size--
	last := heap[size]
	var at int64
	for {
		child := 2*at + 1
		if child >= size {
			break
		}
		if child+1 < size && heap[child+1] < heap[child] {
			child++
		}
		if last <= heap[child] {
			break
		}
		heap[at] = heap[child]
		at = child
	}
	heap[at] = last
	return top
}

func relax(cell, d int64) {
	if d < dist[cell] {
		dist[cell] = d
		push(d*n + cell)
	}
}

func main() {
	var side int64 = 40
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		side = maxSide
	}
	n = side * side
	cost = make([]int64, n)
	dist = make([]int64, n)
	heap = make([]int64, 4*n+1)
	for i := range cost {
		cost[i] = 1 + random()%9
		dist[i] = 1 << 62
	}
	dist[0] = cost[0]
	push(cost[0] * n)
	for size > 0 {
		key := pop()
		d, cell := key/n, key%n
		if d > dist[cell] {
			continue
		}
		row, col := cell/side, cell%side
		if row > 0 {
			relax(cell-side, d+cost[cell-side])
		}
		if row < side-1 {
			relax(cell+side, d+cost[cell+side])
		}
		if col > 0 {
			relax(cell-1, d+cost[cell-1])
		}
		if col < side-1 {
			relax(cell+1, d+cost[cell+1])
		}
	}
	var total int64
	for _, d := range dist {
		total += d
	}
	fmt.Printf("the cheapest path costs %d; the costs to every cell sum to %d\n", dist[n-1], total)
}
