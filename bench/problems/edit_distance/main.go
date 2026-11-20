// Edit distance; see main.wind.
package main

import (
	"fmt"
	"os"
)

const max = 6000

var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func main() {
	n := 300
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = max
	}
	a := make([]byte, n)
	b := make([]byte, n)
	for i := range a {
		a[i] = byte(97 + random()%4)
	}
	for i := range b {
		b[i] = byte(97 + random()%4)
	}
	previous := make([]int64, n+1)
	current := make([]int64, n+1)
	for j := 0; j <= n; j++ {
		previous[j] = int64(j)
	}
	for i := 1; i <= n; i++ {
		current[0] = int64(i)
		for j := 1; j <= n; j++ {
			var cost int64 = 1
			if a[i-1] == b[j-1] {
				cost = 0
			}
			best := previous[j-1] + cost
			if previous[j]+1 < best {
				best = previous[j] + 1
			}
			if current[j-1]+1 < best {
				best = current[j-1] + 1
			}
			current[j] = best
		}
		previous, current = current, previous
	}
	fmt.Println(previous[n])
}
