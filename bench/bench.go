// The benchmark program in Go; see bench.wind for what each kernel measures.
package main

import (
	"fmt"
	"os"
)

type Node struct {
	value int64
	next  *Node
}

type Particle struct {
	x, y, vx, vy float64
}

const (
	sieveLimit    = 20000000
	sortCount     = 2000000
	matrixSize    = 400
	listCount     = 1000000
	hashSize      = 2097152
	hashKeys      = 1000000
	bytesCount    = 4000000
	particleCount = 1000
	queens        = 12
)

var (
	composite [sieveLimit + 1]bool
	numbers   [sortCount]int64
	left      [matrixSize * matrixSize]float64
	right     [matrixSize * matrixSize]float64
	product   [matrixSize * matrixSize]float64
	nodes     [listCount]Node
	order     [listCount]int64
	table     [hashSize]int64
	text      [bytesCount]byte
	counts    [26]int64
	particles [particleCount]Particle
	columns   [queens]bool
	falling   [2 * queens]bool
	rising    [2 * queens]bool
	seed      int64 = 42
)

func random() int64 {
	seed = seed * 48271 % 2147483647
	return seed
}

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func sieve() int64 {
	var count int64
	for i := int64(2); i <= sieveLimit; i++ {
		if composite[i] {
			continue
		}
		count++
		for j := i * 2; j <= sieveLimit; j += i {
			composite[j] = true
		}
	}
	return count
}

func collatz() int64 {
	// Unsigned numbers halve with a shift.
	var best, bestStart uint64
	for start := uint64(1); start < 300000; start++ {
		n := start
		var steps uint64
		for n != 1 {
			if n%2 == 0 {
				n /= 2
			} else {
				n = 3*n + 1
			}
			steps++
		}
		if steps > best {
			best = steps
			bestStart = start
		}
	}
	return int64(bestStart*1000 + best)
}

func gcd(a, b int64) int64 {
	x, y := a, b
	for y != 0 {
		x, y = y, x%y
	}
	return x
}

func gcdSum() int64 {
	var total int64
	for i := int64(1); i <= 1200; i++ {
		for j := int64(1); j <= 1200; j++ {
			total += gcd(i, j)
		}
	}
	return total
}

func mandelbrot() int64 {
	var size int64 = 600
	var total int64
	for py := int64(0); py < size; py++ {
		for px := int64(0); px < size; px++ {
			cx := float64(px)*3.0/float64(size) - 2.0
			cy := float64(py)*2.0/float64(size) - 1.0
			x, y := 0.0, 0.0
			var i int64
			for i < 200 && x*x+y*y <= 4.0 {
				next := x*x - y*y + cx
				y = 2.0*x*y + cy
				x = next
				i++
			}
			total += i
		}
	}
	return total
}

func matmul() int64 {
	var n int64 = matrixSize
	for i := int64(0); i < n*n; i++ {
		left[i] = float64(i%7) + 0.5
		right[i] = float64(i%5) - 1.5
	}
	for i := int64(0); i < n; i++ {
		for j := int64(0); j < n; j++ {
			sum := 0.0
			for k := int64(0); k < n; k++ {
				sum += left[i*n+k] * right[k*n+j]
			}
			product[i*n+j] = sum
		}
	}
	total := 0.0
	for i := int64(0); i < n*n; i++ {
		total += product[i]
	}
	return int64(total)
}

func particleSum() int64 {
	for i := 0; i < particleCount; i++ {
		p := &particles[i]
		p.x = float64(random() % 1000)
		p.y = float64(random() % 1000)
		p.vx = float64(random()%200-100) / 8.0
		p.vy = float64(random()%200-100) / 8.0
	}
	for step := 0; step < 20000; step++ {
		for i := 0; i < particleCount; i++ {
			p := &particles[i]
			p.x += p.vx * 0.5
			p.y += p.vy * 0.5
			if p.x < 0.0 || p.x > 1000.0 {
				p.vx = -p.vx
			}
			if p.y < 0.0 || p.y > 1000.0 {
				p.vy = -p.vy
			}
		}
	}
	total := 0.0
	for i := 0; i < particleCount; i++ {
		total += particles[i].x + particles[i].y
	}
	return int64(total)
}

func quicksort(values []int64, low, high int64) {
	if low >= high {
		return
	}
	pivot := values[(low+high)/2]
	i, j := low, high
	for i <= j {
		for values[i] < pivot {
			i++
		}
		for values[j] > pivot {
			j--
		}
		if i <= j {
			values[i], values[j] = values[j], values[i]
			i++
			j--
		}
	}
	quicksort(values, low, j)
	quicksort(values, i, high)
}

func sortNumbers() int64 {
	for i := int64(0); i < sortCount; i++ {
		numbers[i] = random() % 1000000
	}
	quicksort(numbers[:], 0, sortCount-1)
	var check int64
	for i := int64(0); i < sortCount; i += 1000 {
		check += numbers[i] * (i%7 + 1)
	}
	return check
}

func place(row int64) int64 {
	if row == queens {
		return 1
	}
	var ways int64
	for column := int64(0); column < queens; column++ {
		down := row + column
		up := row - column + queens
		if columns[column] || falling[down] || rising[up] {
			continue
		}
		columns[column] = true
		falling[down] = true
		rising[up] = true
		ways += place(row + 1)
		columns[column] = false
		falling[down] = false
		rising[up] = false
	}
	return ways
}

func hash() int64 {
	for i := 0; i < hashKeys; i++ {
		key := random()
		slot := key % hashSize
		for table[slot] != 0 && table[slot] != key {
			slot = (slot + 1) % hashSize
		}
		table[slot] = key
	}
	seed = 42
	var found, probes int64
	for i := int64(0); i < hashKeys; i++ {
		key := random() + i%2
		slot := key % hashSize
		for table[slot] != 0 && table[slot] != key {
			slot = (slot + 1) % hashSize
			probes++
		}
		if table[slot] == key {
			found++
		}
	}
	return found*100 + probes
}

func bytes() int64 {
	for i := 0; i < bytesCount; i++ {
		text[i] = byte(97 + random()%26)
	}
	var code int64
	for i := 0; i < bytesCount; i++ {
		letter := int64(text[i])
		counts[letter-97]++
		code = (code*31 + letter) % 1000000007
	}
	var pairs int64
	for i := 1; i < bytesCount; i++ {
		if text[i-1] == 'w' && text[i] == 'd' {
			pairs++
		}
	}
	return code + counts[4] + pairs*7
}

func list() int64 {
	for i := int64(0); i < listCount; i++ {
		order[i] = i
		nodes[i].value = i % 100
	}
	for i := int64(listCount - 1); i > 0; i-- {
		j := random() % (i + 1)
		order[i], order[j] = order[j], order[i]
	}
	for i := int64(0); i < listCount-1; i++ {
		nodes[order[i]].next = &nodes[order[i+1]]
	}
	nodes[order[listCount-1]].next = nil

	var total int64
	for pass := int64(0); pass < 20; pass++ {
		for at := &nodes[order[0]]; at != nil; at = at.next {
			total += at.value + pass
		}
	}
	return total
}

func main() {
	kernel := ""
	if len(os.Args) > 1 {
		kernel = os.Args[1]
	}
	var result int64
	switch kernel {
	case "fib":
		result = fib(38)
	case "sieve":
		result = sieve()
	case "collatz":
		result = collatz()
	case "gcd":
		result = gcdSum()
	case "mandelbrot":
		result = mandelbrot()
	case "matmul":
		result = matmul()
	case "particles":
		result = particleSum()
	case "sort":
		result = sortNumbers()
	case "queens":
		result = place(0)
	case "hash":
		result = hash()
	case "bytes":
		result = bytes()
	case "list":
		result = list()
	default:
		fmt.Printf("unknown kernel '%s'\n", kernel)
		os.Exit(2)
	}
	fmt.Println(result)
}
