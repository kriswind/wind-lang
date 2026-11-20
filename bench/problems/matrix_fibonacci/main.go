// Matrix Fibonacci; see main.wind.
package main

import (
	"fmt"
	"os"
)

const prime = 67108859

type matrix struct {
	a, b, c, d int64
}

var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func multiply(x, y matrix) matrix {
	return matrix{
		(x.a*y.a + x.b*y.c) % prime, (x.a*y.b + x.b*y.d) % prime,
		(x.c*y.a + x.d*y.c) % prime, (x.c*y.b + x.d*y.d) % prime,
	}
}

func fibonacci(n int64) int64 {
	result := matrix{1, 0, 0, 1}
	base := matrix{1, 1, 1, 0}
	left := n
	for left > 0 {
		if left&1 == 1 {
			result = multiply(result, base)
		}
		base = multiply(base, base)
		left >>= 1
	}
	return result.b
}

func main() {
	queries := 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		queries = 1000000
	}
	var total int64
	for i := 0; i < queries; i++ {
		total = (total + fibonacci(random())) % prime
	}
	fmt.Println(total)
}
