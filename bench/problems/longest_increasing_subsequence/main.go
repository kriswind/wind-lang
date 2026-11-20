// Longest increasing subsequence; see main.wind.
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

func main() {
	n := 5000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 3000000
	}
	values := make([]int64, n)
	for i := range values {
		values[i] = random() % 1000000
	}
	tails := make([]int64, n)
	length := 0
	for _, x := range values {
		low, high := 0, length
		for low < high {
			middle := (low + high) / 2
			if tails[middle] < x {
				low = middle + 1
			} else {
				high = middle
			}
		}
		tails[low] = x
		if low == length {
			length++
		}
	}
	var sum int64
	for _, t := range tails[:length] {
		sum += t
	}
	fmt.Printf("length %d\n", length)
	fmt.Printf("tails %d\n", sum)
}
