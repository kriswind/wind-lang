// Topological sort; see main.wind.
package main

import (
	"fmt"
	"os"
)

const max = 1000000

var heap []int64
var size int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func push(node int64) {
	at := size
	size++
	for at > 0 {
		parent := (at - 1) / 2
		if heap[parent] <= node {
			break
		}
		heap[at] = heap[parent]
		at = parent
	}
	heap[at] = node
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

func main() {
	var n int64 = 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = max
	}
	m := 4 * n
	label := make([]int64, n)
	for i := range label {
		label[i] = int64(i)
	}
	for i := n - 1; i > 0; i-- {
		j := random() % (i + 1)
		label[i], label[j] = label[j], label[i]
	}
	source := make([]int64, m)
	target := make([]int64, m)
	var edges int64
	for e := int64(0); e < m; e++ {
		a := random() % n
		b := random() % n
		if a == b {
			continue
		}
		if a > b {
			a, b = b, a
		}
		source[edges] = label[a]
		target[edges] = label[b]
		edges++
	}
	first := make([]int64, n+1)
	waiting := make([]int64, n)
	for e := int64(0); e < edges; e++ {
		first[source[e]+1]++
		waiting[target[e]]++
	}
	for i := int64(0); i < n; i++ {
		first[i+1] += first[i]
	}
	fill := make([]int64, n)
	copy(fill, first[:n])
	after := make([]int64, edges)
	for e := int64(0); e < edges; e++ {
		after[fill[source[e]]] = target[e]
		fill[source[e]]++
	}

	heap = make([]int64, n)
	for i := int64(0); i < n; i++ {
		if waiting[i] == 0 {
			push(i)
		}
	}
	var placed, checksum int64
	for size > 0 {
		node := pop()
		placed++
		checksum = (checksum*31 + node) % 1000000007
		for k := first[node]; k < first[node+1]; k++ {
			v := after[k]
			waiting[v]--
			if waiting[v] == 0 {
				push(v)
			}
		}
	}
	fmt.Printf("%d edges, %d nodes placed, checksum %d\n", edges, placed, checksum)
}
