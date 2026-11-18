// Merge sort inversions; see main.wind.
package main

import (
	"fmt"
	"os"
)

var values, scratch []int64
var seed int64 = 42

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func merge(low, middle, high int) int64 {
	i, j, k := low, middle, low
	var inversions int64
	for i < middle && j < high {
		if values[j] < values[i] {
			scratch[k] = values[j]
			j++
			inversions += int64(middle - i)
		} else {
			scratch[k] = values[i]
			i++
		}
		k++
	}
	for i < middle {
		scratch[k] = values[i]
		i++
		k++
	}
	for j < high {
		scratch[k] = values[j]
		j++
		k++
	}
	copy(values[low:high], scratch[low:high])
	return inversions
}

func sort(n int) int64 {
	var inversions int64
	for width := 1; width < n; width *= 2 {
		for low := 0; low < n-width; low += 2 * width {
			high := low + 2*width
			if high > n {
				high = n
			}
			inversions += merge(low, low+width, high)
		}
	}
	return inversions
}

func main() {
	n := 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 2000000
	}
	values = make([]int64, n)
	scratch = make([]int64, n)
	for i := range values {
		values[i] = random() % int64(n)
	}
	inversions := sort(n)
	var checksum int64
	for _, v := range values {
		checksum = (checksum*31 + v) % 1000000007
	}
	fmt.Printf("%d inversions, checksum %d\n", inversions, checksum)
}
