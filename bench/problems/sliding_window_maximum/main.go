// Sliding window maximum; see main.wind.
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
	bench := len(os.Args) > 1 && os.Args[1] == "bench"
	n, k := 1000, 16
	if bench {
		n, k = 10000000, 1000
	}
	values := make([]int64, n)
	for i := range values {
		values[i] = random() % 1000000
	}
	deque := make([]int, n)
	head, tail := 0, 0
	var sum int64
	for i := 0; i < n; i++ {
		for tail > head && values[deque[tail-1]] <= values[i] {
			tail--
		}
		deque[tail] = i
		tail++
		if deque[head] <= i-k {
			head++
		}
		if i >= k-1 {
			sum += values[deque[head]]
		}
	}
	fmt.Printf("%d windows of %d, the maxima summing to %d\n", n-k+1, k, sum)
}
