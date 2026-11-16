// The benchmark program in Odin; see bench.wind for what each kernel measures.
// It is built for speed, without bounds checks.
package main

import "core:fmt"
import "core:os"

Node :: struct {
	value: int,
	next:  ^Node,
}

Particle :: struct {
	x, y, vx, vy: f64,
}

SIEVE_LIMIT :: 20000000
SORT_COUNT :: 2000000
MATRIX_SIZE :: 400
LIST_COUNT :: 1000000
HASH_SIZE :: 2097152
HASH_KEYS :: 1000000
BYTES_COUNT :: 4000000
PARTICLE_COUNT :: 1000
QUEENS :: 12

composite: [SIEVE_LIMIT + 1]bool
numbers: [SORT_COUNT]int
left: [MATRIX_SIZE * MATRIX_SIZE]f64
right: [MATRIX_SIZE * MATRIX_SIZE]f64
product: [MATRIX_SIZE * MATRIX_SIZE]f64
nodes: [LIST_COUNT]Node
order: [LIST_COUNT]int
table: [HASH_SIZE]int
text: [BYTES_COUNT]u8
counts: [26]int
particles: [PARTICLE_COUNT]Particle
columns: [QUEENS]bool
falling: [2 * QUEENS]bool
rising: [2 * QUEENS]bool

seed := 42

random :: proc() -> int {
	seed = seed * 48271 % 2147483647
	return seed
}

fib :: proc(n: int) -> int {
	if n < 2 {
		return n
	}
	return fib(n - 1) + fib(n - 2)
}

sieve :: proc() -> int {
	count := 0
	for i := 2; i <= SIEVE_LIMIT; i += 1 {
		if composite[i] {
			continue
		}
		count += 1
		for j := i * 2; j <= SIEVE_LIMIT; j += i {
			composite[j] = true
		}
	}
	return count
}

collatz :: proc() -> int {
	best := 0
	best_start := 0
	for start := 1; start < 300000; start += 1 {
		n := start
		steps := 0
		for n != 1 {
			if n % 2 == 0 {
				n /= 2
			} else {
				n = 3 * n + 1
			}
			steps += 1
		}
		if steps > best {
			best = steps
			best_start = start
		}
	}
	return best_start * 1000 + best
}

gcd :: proc(a, b: int) -> int {
	x := a
	y := b
	for y != 0 {
		rest := x % y
		x = y
		y = rest
	}
	return x
}

gcd_sum :: proc() -> int {
	total := 0
	for i := 1; i <= 1200; i += 1 {
		for j := 1; j <= 1200; j += 1 {
			total += gcd(i, j)
		}
	}
	return total
}

mandelbrot :: proc() -> int {
	size := 600
	total := 0
	for py := 0; py < size; py += 1 {
		for px := 0; px < size; px += 1 {
			cx := f64(px) * 3.0 / f64(size) - 2.0
			cy := f64(py) * 2.0 / f64(size) - 1.0
			x := 0.0
			y := 0.0
			i := 0
			for i < 200 && x * x + y * y <= 4.0 {
				next := x * x - y * y + cx
				y = 2.0 * x * y + cy
				x = next
				i += 1
			}
			total += i
		}
	}
	return total
}

matmul :: proc() -> int {
	n := MATRIX_SIZE
	for i := 0; i < n * n; i += 1 {
		left[i] = f64(i % 7) + 0.5
		right[i] = f64(i % 5) - 1.5
	}
	for i := 0; i < n; i += 1 {
		for j := 0; j < n; j += 1 {
			sum := 0.0
			for k := 0; k < n; k += 1 {
				sum += left[i * n + k] * right[k * n + j]
			}
			product[i * n + j] = sum
		}
	}
	total := 0.0
	for i := 0; i < n * n; i += 1 {
		total += product[i]
	}
	return int(total)
}

particle_sum :: proc() -> int {
	for i := 0; i < PARTICLE_COUNT; i += 1 {
		p := &particles[i]
		p.x = f64(random() % 1000)
		p.y = f64(random() % 1000)
		p.vx = f64(random() % 200 - 100) / 8.0
		p.vy = f64(random() % 200 - 100) / 8.0
	}
	for step := 0; step < 20000; step += 1 {
		for i := 0; i < PARTICLE_COUNT; i += 1 {
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
	for i := 0; i < PARTICLE_COUNT; i += 1 {
		total += particles[i].x + particles[i].y
	}
	return int(total)
}

quicksort :: proc(values: []int, low, high: int) {
	if low >= high {
		return
	}
	pivot := values[(low + high) / 2]
	i := low
	j := high
	for i <= j {
		for values[i] < pivot {
			i += 1
		}
		for values[j] > pivot {
			j -= 1
		}
		if i <= j {
			values[i], values[j] = values[j], values[i]
			i += 1
			j -= 1
		}
	}
	quicksort(values, low, j)
	quicksort(values, i, high)
}

sort :: proc() -> int {
	for i := 0; i < SORT_COUNT; i += 1 {
		numbers[i] = random() % 1000000
	}
	quicksort(numbers[:], 0, SORT_COUNT - 1)
	check := 0
	for i := 0; i < SORT_COUNT; i += 1000 {
		check += numbers[i] * (i % 7 + 1)
	}
	return check
}

place :: proc(row: int) -> int {
	if row == QUEENS {
		return 1
	}
	ways := 0
	for column := 0; column < QUEENS; column += 1 {
		down := row + column
		up := row - column + QUEENS
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

hash :: proc() -> int {
	for i := 0; i < HASH_KEYS; i += 1 {
		key := random()
		slot := key % HASH_SIZE
		for table[slot] != 0 && table[slot] != key {
			slot = (slot + 1) % HASH_SIZE
		}
		table[slot] = key
	}
	seed = 42
	found := 0
	probes := 0
	for i := 0; i < HASH_KEYS; i += 1 {
		key := random() + i % 2
		slot := key % HASH_SIZE
		for table[slot] != 0 && table[slot] != key {
			slot = (slot + 1) % HASH_SIZE
			probes += 1
		}
		if table[slot] == key {
			found += 1
		}
	}
	return found * 100 + probes
}

bytes :: proc() -> int {
	for i := 0; i < BYTES_COUNT; i += 1 {
		text[i] = u8(97 + random() % 26)
	}
	code := 0
	for i := 0; i < BYTES_COUNT; i += 1 {
		letter := int(text[i])
		counts[letter - 97] += 1
		code = (code * 31 + letter) % 1000000007
	}
	pairs := 0
	for i := 1; i < BYTES_COUNT; i += 1 {
		if text[i - 1] == 'w' && text[i] == 'd' {
			pairs += 1
		}
	}
	return code + counts[4] + pairs * 7
}

list :: proc() -> int {
	for i := 0; i < LIST_COUNT; i += 1 {
		order[i] = i
		nodes[i].value = i % 100
	}
	for i := LIST_COUNT - 1; i > 0; i -= 1 {
		j := random() % (i + 1)
		order[i], order[j] = order[j], order[i]
	}
	for i := 0; i < LIST_COUNT - 1; i += 1 {
		nodes[order[i]].next = &nodes[order[i + 1]]
	}
	nodes[order[LIST_COUNT - 1]].next = nil

	total := 0
	for pass := 0; pass < 20; pass += 1 {
		for at := &nodes[order[0]]; at != nil; at = at.next {
			total += at.value + pass
		}
	}
	return total
}

main :: proc() {
	kernel := len(os.args) > 1 ? os.args[1] : ""
	result: int
	switch kernel {
	case "fib":
		result = fib(38)
	case "sieve":
		result = sieve()
	case "collatz":
		result = collatz()
	case "gcd":
		result = gcd_sum()
	case "mandelbrot":
		result = mandelbrot()
	case "matmul":
		result = matmul()
	case "particles":
		result = particle_sum()
	case "sort":
		result = sort()
	case "queens":
		result = place(0)
	case "hash":
		result = hash()
	case "bytes":
		result = bytes()
	case "list":
		result = list()
	case:
		fmt.printf("unknown kernel '%s'\n", kernel)
		os.exit(2)
	}
	fmt.println(result)
}
