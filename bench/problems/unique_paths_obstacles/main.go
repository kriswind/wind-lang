// Unique paths with obstacles; see main.wind.
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

func main() {
	size := 120
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		size = 5000
	}
	rows, columns := size, size
	paths := make([]int64, columns)
	obstacles := 0
	for i := 0; i < rows; i++ {
		for j := 0; j < columns; j++ {
			corner := (i == 0 && j == 0) || (i == rows-1 && j == columns-1)
			if random()%10 == 0 && !corner {
				obstacles++
				paths[j] = 0
			} else if i == 0 && j == 0 {
				paths[j] = 1
			} else if j > 0 {
				total := paths[j] + paths[j-1]
				if total >= prime {
					total -= prime
				}
				paths[j] = total
			}
		}
	}
	fmt.Printf("paths %d\n", paths[columns-1])
	fmt.Printf("obstacles %d\n", obstacles)
}
