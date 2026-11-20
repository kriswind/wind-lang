// Longest common subsequence; see main.wind.
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
	n := 400
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 8000
	}
	m := n * 3 / 4
	a := make([]byte, n)
	b := make([]byte, m)
	for i := range a {
		a[i] = byte(97 + random()%8)
	}
	for j := range b {
		b[j] = byte(97 + random()%8)
	}
	previous := make([]int64, m+1)
	current := make([]int64, m+1)
	for i := 1; i <= n; i++ {
		letter := a[i-1]
		for j := 1; j <= m; j++ {
			if letter == b[j-1] {
				current[j] = previous[j-1] + 1
			} else if previous[j] > current[j-1] {
				current[j] = previous[j]
			} else {
				current[j] = current[j-1]
			}
		}
		previous, current = current, previous
	}
	fmt.Println(previous[m])
}
