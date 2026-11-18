// Trapping rain water; see main.wind.
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

func trapped(heights []int64) int64 {
	left, right := 0, len(heights)-1
	var leftMax, rightMax, water int64
	for left < right {
		if heights[left] < heights[right] {
			if heights[left] >= leftMax {
				leftMax = heights[left]
			} else {
				water += leftMax - heights[left]
			}
			left++
		} else {
			if heights[right] >= rightMax {
				rightMax = heights[right]
			} else {
				water += rightMax - heights[right]
			}
			right--
		}
	}
	return water
}

func main() {
	n := 1000
	if len(os.Args) > 1 && os.Args[1] == "bench" {
		n = 10000000
	}
	heights := make([]int64, n)
	for i := range heights {
		heights[i] = random() % 1000
	}
	fmt.Println(trapped(heights))
}
