// Longest unique substring; see main.wind.
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
	n := 10000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 20000000
	}
	text := make([]byte, n)
	for i := range text {
		text[i] = byte(97 + random()%26)
	}
	var last [26]int
	for c := range last {
		last[c] = -1
	}
	start, best, bestStart := 0, 0, 0
	for i := 0; i < n; i++ {
		c := text[i] - 97
		if last[c] >= start {
			start = last[c] + 1
		}
		last[c] = i
		if i-start+1 > best {
			best = i - start + 1
			bestStart = start
		}
	}
	fmt.Printf("%d letters from %d: %s\n", best, bestStart, text[bestStart:bestStart+best])
}
