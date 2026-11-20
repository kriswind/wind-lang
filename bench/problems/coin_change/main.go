// Coin change; see main.wind.
package main

import (
	"fmt"
	"os"
)

const prime = 1000000007

var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func chosen(coins []int64, value int64) bool {
	for _, c := range coins {
		if c == value {
			return true
		}
	}
	return false
}

func main() {
	bench := len(os.Args) > 1 && os.Args[1] == "bench"
	count, limit := 8, int64(2000)
	if bench {
		count, limit = 40, 1000000
	}
	coins := make([]int64, 0, count)
	for i := 0; i < count; i++ {
		coin := 2 + random()%199
		for chosen(coins, coin) {
			coin = 2 + random()%199
		}
		coins = append(coins, coin)
	}

	ways := make([]int64, limit+1)
	ways[0] = 1
	for _, coin := range coins {
		for a := coin; a <= limit; a++ {
			total := ways[a] + ways[a-coin]
			if total >= prime {
				total -= prime
			}
			ways[a] = total
		}
	}

	fewest := make([]int64, limit+1)
	for a := int64(1); a <= limit; a++ {
		fewest[a] = limit + 1
	}
	for _, coin := range coins {
		for a := coin; a <= limit; a++ {
			used := fewest[a-coin] + 1
			if used < fewest[a] {
				fewest[a] = used
			}
		}
	}

	var waysSum, fewestSum, unreachable int64
	for a := int64(1); a <= limit; a++ {
		waysSum += ways[a]
		if waysSum >= prime {
			waysSum -= prime
		}
		if fewest[a] > limit {
			unreachable++
		} else {
			fewestSum += fewest[a]
		}
	}
	fmt.Printf("ways %d\n", waysSum)
	fmt.Printf("fewest %d\n", fewestSum)
	fmt.Printf("unreachable %d\n", unreachable)
}
