// Maximum subarray; see main.wind.
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
	n := 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 10000000
	}
	values := make([]int64, n)
	for i := range values {
		values[i] = random()%2001 - 1000
	}
	best, bestStart, bestEnd := values[0], 0, 0
	sum, start := values[0], 0
	for i := 1; i < n; i++ {
		if sum <= 0 {
			sum = values[i]
			start = i
		} else {
			sum += values[i]
		}
		if sum > best {
			best = sum
			bestStart = start
			bestEnd = i
		}
	}
	fmt.Printf("%d from %d to %d\n", best, bestStart, bestEnd)
}
