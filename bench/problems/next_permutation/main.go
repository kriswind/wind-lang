// Next permutation; see main.wind.
package main

import (
	"fmt"
	"os"
)

var items, weights []int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func nextPermutation(n int) bool {
	i := n - 2
	for i >= 0 && items[i] >= items[i+1] {
		i--
	}
	if i < 0 {
		return false
	}
	j := n - 1
	for items[j] <= items[i] {
		j--
	}
	items[i], items[j] = items[j], items[i]
	for low, high := i+1, n-1; low < high; low, high = low+1, high-1 {
		items[low], items[high] = items[high], items[low]
	}
	return true
}

func main() {
	n, rounds := 7, 2
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n, rounds = 10, 4
	}
	items = make([]int64, n)
	weights = make([]int64, n)
	var count, total, checksum int64
	for round := 0; round < rounds; round++ {
		for i := 0; i < n; i++ {
			items[i] = int64(i)
			weights[i] = random()%1000 + 1
		}
		for {
			var value int64
			for i := 0; i < n; i++ {
				value += items[i] * weights[i]
			}
			count++
			total += value
			checksum = (checksum*31 + value) % 1000000007
			if !nextPermutation(n) {
				break
			}
		}
	}
	fmt.Printf("%d permutations, total %d, checksum %d\n", count, total, checksum)
}
