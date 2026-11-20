// Kth largest; see main.wind.
package main

import (
	"fmt"
	"os"
)

var heap []int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func push(value int64) {
	at := len(heap)
	heap = append(heap, value)
	for at > 0 {
		parent := (at - 1) / 2
		if heap[parent] <= heap[at] {
			break
		}
		heap[parent], heap[at] = heap[at], heap[parent]
		at = parent
	}
}

func replaceRoot(value int64) {
	heap[0] = value
	size := len(heap)
	at := 0
	for {
		child := 2*at + 1
		if child >= size {
			break
		}
		if child+1 < size && heap[child+1] < heap[child] {
			child++
		}
		if heap[at] <= heap[child] {
			break
		}
		heap[child], heap[at] = heap[at], heap[child]
		at = child
	}
}

func main() {
	bench := len(os.Args) > 1 && os.Args[1] == "bench"
	n, k := 10000, 100
	if bench {
		n, k = 30000000, 100000
	}
	heap = make([]int64, 0, k)
	for i := 0; i < n; i++ {
		value := random()
		if len(heap) < k {
			push(value)
		} else if value > heap[0] {
			replaceRoot(value)
		}
	}
	var sum int64
	for _, v := range heap {
		sum += v
	}
	fmt.Printf("sum %d, k-th largest %d\n", sum, heap[0])
}
