// Union-find; see main.wind.
package main

import (
	"fmt"
	"os"
)

var parent, size []int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func find(node int64) int64 {
	at := node
	for parent[at] != at {
		parent[at] = parent[parent[at]]
		at = parent[at]
	}
	return at
}

func join(a, b int64) bool {
	x, y := find(a), find(b)
	if x == y {
		return false
	}
	if size[x] < size[y] {
		x, y = y, x
	}
	parent[y] = x
	size[x] += size[y]
	return true
}

func main() {
	var n int64 = 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 2000000
	}
	parent = make([]int64, n)
	size = make([]int64, n)
	for i := int64(0); i < n; i++ {
		parent[i] = i
		size[i] = 1
	}
	groups := n
	for i := int64(0); i < n*9/10; i++ {
		a := random() % n
		b := random() % n
		if join(a, b) {
			groups--
		}
	}
	var largest int64
	for i := int64(0); i < n; i++ {
		if parent[i] == i && size[i] > largest {
			largest = size[i]
		}
	}
	fmt.Printf("%d groups, the largest of %d\n", groups, largest)
}
