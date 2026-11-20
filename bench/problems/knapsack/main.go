// 0/1 knapsack; see main.wind.
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
	var n int64 = 60
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 1000
	}
	capacity := n * 125
	weight := make([]int64, n)
	value := make([]int64, n)
	for i := range weight {
		weight[i] = 1 + random()%1000
		value[i] = 1 + random()%1000
	}
	best := make([]int64, capacity+1)
	for i := range weight {
		w, v := weight[i], value[i]
		for c := capacity; c >= w; c-- {
			taken := best[c-w] + v
			if taken > best[c] {
				best[c] = taken
			}
		}
	}
	fmt.Println(best[capacity])
}
