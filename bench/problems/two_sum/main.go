// Two sum; see main.wind.
package main

import (
	"fmt"
	"os"
)

var keys, counts []int64
var mask int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

// slot gives the slot holding key, or the empty slot (key -1) where it would go.
func slot(key int64) int64 {
	at := (key * 2654435761) & mask
	for keys[at] != -1 && keys[at] != key {
		at = (at + 1) & mask
	}
	return at
}

func main() {
	var n int64 = 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 2000000
	}
	var size int64 = 1
	for size < 2*n {
		size *= 2
	}
	mask = size - 1
	keys = make([]int64, size)
	counts = make([]int64, size)
	for i := range keys {
		keys[i] = -1
	}
	values := make([]int64, n)
	for i := range values {
		values[i] = random() % n
	}
	var pairs, distinct int64
	for _, value := range values {
		want := n - value
		at := slot(want)
		if keys[at] == want {
			pairs += counts[at]
		}
		at = slot(value)
		if keys[at] != value {
			keys[at] = value
			distinct++
		}
		counts[at]++
	}
	fmt.Printf("%d pairs, %d distinct values\n", pairs, distinct)
}
