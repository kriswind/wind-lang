// The benchmark program in Rust; see bench.wind for what each kernel measures.
// It is written for speed: the loops that index heavily do it without bounds
// checks, and the linked list is made of raw pointers, both inside "unsafe".

use std::env;

const SIEVE_LIMIT: usize = 20000000;
const SORT_COUNT: usize = 2000000;
const MATRIX_SIZE: usize = 400;
const LIST_COUNT: usize = 1000000;
const HASH_SIZE: usize = 2097152;
const HASH_KEYS: usize = 1000000;
const BYTES_COUNT: usize = 4000000;
const PARTICLE_COUNT: usize = 1000;
const QUEENS: usize = 12;

#[derive(Clone)]
struct Node {
    value: i64,
    next: *const Node,
}

#[derive(Clone)]
struct Particle {
    x: f64,
    y: f64,
    vx: f64,
    vy: f64,
}

struct Random {
    seed: i64,
}

impl Random {
    fn next(&mut self) -> i64 {
        self.seed = self.seed * 48271 % 2147483647;
        self.seed
    }
}

fn fib(n: i64) -> i64 {
    if n < 2 {
        return n;
    }
    fib(n - 1) + fib(n - 2)
}

fn sieve() -> i64 {
    let mut composite = vec![false; SIEVE_LIMIT + 1];
    let mut count = 0;
    for i in 2..=SIEVE_LIMIT {
        if unsafe { *composite.get_unchecked(i) } {
            continue;
        }
        count += 1;
        let mut j = i * 2;
        while j <= SIEVE_LIMIT {
            unsafe { *composite.get_unchecked_mut(j) = true };
            j += i;
        }
    }
    count
}

fn collatz() -> i64 {
    let mut best = 0;
    let mut best_start = 0;
    for start in 1..300000i64 {
        let mut n = start;
        let mut steps = 0;
        while n != 1 {
            if n % 2 == 0 {
                n /= 2;
            } else {
                n = 3 * n + 1;
            }
            steps += 1;
        }
        if steps > best {
            best = steps;
            best_start = start;
        }
    }
    best_start * 1000 + best
}

fn gcd(a: i64, b: i64) -> i64 {
    let mut x = a;
    let mut y = b;
    while y != 0 {
        let rest = x % y;
        x = y;
        y = rest;
    }
    x
}

fn gcd_sum() -> i64 {
    let mut total = 0;
    for i in 1..=1200 {
        for j in 1..=1200 {
            total += gcd(i, j);
        }
    }
    total
}

fn mandelbrot() -> i64 {
    let size = 600i64;
    let mut total = 0;
    for py in 0..size {
        for px in 0..size {
            let cx = px as f64 * 3.0 / size as f64 - 2.0;
            let cy = py as f64 * 2.0 / size as f64 - 1.0;
            let mut x = 0.0;
            let mut y = 0.0;
            let mut i = 0;
            while i < 200 && x * x + y * y <= 4.0 {
                let next = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = next;
                i += 1;
            }
            total += i;
        }
    }
    total
}

fn matmul() -> i64 {
    let n = MATRIX_SIZE;
    let mut left = vec![0.0f64; n * n];
    let mut right = vec![0.0f64; n * n];
    let mut product = vec![0.0f64; n * n];
    for i in 0..n * n {
        left[i] = (i % 7) as f64 + 0.5;
        right[i] = (i % 5) as f64 - 1.5;
    }
    for i in 0..n {
        for j in 0..n {
            let mut sum = 0.0;
            for k in 0..n {
                sum += left[i * n + k] * right[k * n + j];
            }
            product[i * n + j] = sum;
        }
    }
    let mut total = 0.0;
    for i in 0..n * n {
        total += product[i];
    }
    total as i64
}

fn particle_sum(random: &mut Random) -> i64 {
    let mut particles = vec![Particle { x: 0.0, y: 0.0, vx: 0.0, vy: 0.0 }; PARTICLE_COUNT];
    for p in particles.iter_mut() {
        p.x = (random.next() % 1000) as f64;
        p.y = (random.next() % 1000) as f64;
        p.vx = (random.next() % 200 - 100) as f64 / 8.0;
        p.vy = (random.next() % 200 - 100) as f64 / 8.0;
    }
    for _ in 0..20000 {
        for p in particles.iter_mut() {
            p.x += p.vx * 0.5;
            p.y += p.vy * 0.5;
            if p.x < 0.0 || p.x > 1000.0 {
                p.vx = -p.vx;
            }
            if p.y < 0.0 || p.y > 1000.0 {
                p.vy = -p.vy;
            }
        }
    }
    let mut total = 0.0;
    for p in particles.iter() {
        total += p.x + p.y;
    }
    total as i64
}

fn quicksort(values: *mut i64, low: isize, high: isize) {
    if low >= high {
        return;
    }
    unsafe {
        let pivot = *values.offset((low + high) / 2);
        let mut i = low;
        let mut j = high;
        while i <= j {
            while *values.offset(i) < pivot {
                i += 1;
            }
            while *values.offset(j) > pivot {
                j -= 1;
            }
            if i <= j {
                std::ptr::swap(values.offset(i), values.offset(j));
                i += 1;
                j -= 1;
            }
        }
        quicksort(values, low, j);
        quicksort(values, i, high);
    }
}

fn sort(random: &mut Random) -> i64 {
    let mut numbers = vec![0i64; SORT_COUNT];
    for i in 0..SORT_COUNT {
        numbers[i] = random.next() % 1000000;
    }
    quicksort(numbers.as_mut_ptr(), 0, SORT_COUNT as isize - 1);
    let mut check = 0;
    let mut i = 0;
    while i < SORT_COUNT {
        check += numbers[i] * (i as i64 % 7 + 1);
        i += 1000;
    }
    check
}

struct Board {
    columns: [bool; QUEENS],
    falling: [bool; 2 * QUEENS],
    rising: [bool; 2 * QUEENS],
}

fn place(board: &mut Board, row: usize) -> i64 {
    if row == QUEENS {
        return 1;
    }
    let mut ways = 0;
    for column in 0..QUEENS {
        let down = row + column;
        let up = row + QUEENS - column;
        if board.columns[column] || board.falling[down] || board.rising[up] {
            continue;
        }
        board.columns[column] = true;
        board.falling[down] = true;
        board.rising[up] = true;
        ways += place(board, row + 1);
        board.columns[column] = false;
        board.falling[down] = false;
        board.rising[up] = false;
    }
    ways
}

fn hash(random: &mut Random) -> i64 {
    let mut table = vec![0i64; HASH_SIZE];
    let slots = table.as_mut_ptr();
    for _ in 0..HASH_KEYS {
        let key = random.next();
        let mut slot = key as usize % HASH_SIZE;
        unsafe {
            while *slots.add(slot) != 0 && *slots.add(slot) != key {
                slot = (slot + 1) % HASH_SIZE;
            }
            *slots.add(slot) = key;
        }
    }
    random.seed = 42;
    let mut found = 0;
    let mut probes = 0;
    for i in 0..HASH_KEYS as i64 {
        let key = random.next() + i % 2;
        let mut slot = key as usize % HASH_SIZE;
        unsafe {
            while *slots.add(slot) != 0 && *slots.add(slot) != key {
                slot = (slot + 1) % HASH_SIZE;
                probes += 1;
            }
            if *slots.add(slot) == key {
                found += 1;
            }
        }
    }
    found * 100 + probes
}

fn bytes(random: &mut Random) -> i64 {
    let mut text = vec![0u8; BYTES_COUNT];
    for i in 0..BYTES_COUNT {
        text[i] = (97 + random.next() % 26) as u8;
    }
    let mut counts = [0i64; 26];
    let mut code = 0i64;
    for i in 0..BYTES_COUNT {
        let letter = text[i] as i64;
        counts[(letter - 97) as usize] += 1;
        code = (code * 31 + letter) % 1000000007;
    }
    let mut pairs = 0;
    for i in 1..BYTES_COUNT {
        if text[i - 1] == b'w' && text[i] == b'd' {
            pairs += 1;
        }
    }
    code + counts[4] + pairs * 7
}

fn list(random: &mut Random) -> i64 {
    let mut nodes = vec![Node { value: 0, next: std::ptr::null() }; LIST_COUNT];
    let mut order = vec![0usize; LIST_COUNT];
    for i in 0..LIST_COUNT {
        order[i] = i;
        nodes[i].value = (i % 100) as i64;
    }
    for i in (1..LIST_COUNT).rev() {
        let j = random.next() as usize % (i + 1);
        order.swap(i, j);
    }
    for i in 0..LIST_COUNT - 1 {
        let next: *const Node = &nodes[order[i + 1]];
        nodes[order[i]].next = next;
    }
    nodes[order[LIST_COUNT - 1]].next = std::ptr::null();

    let mut total = 0;
    for pass in 0..20 {
        let mut at: *const Node = &nodes[order[0]];
        while !at.is_null() {
            unsafe {
                total += (*at).value + pass;
                at = (*at).next;
            }
        }
    }
    total
}

fn main() {
    let kernel = env::args().nth(1).unwrap_or_default();
    let mut random = Random { seed: 42 };
    let result = match kernel.as_str() {
        "fib" => fib(38),
        "sieve" => sieve(),
        "collatz" => collatz(),
        "gcd" => gcd_sum(),
        "mandelbrot" => mandelbrot(),
        "matmul" => matmul(),
        "particles" => particle_sum(&mut random),
        "sort" => sort(&mut random),
        "queens" => {
            let mut board = Board {
                columns: [false; QUEENS],
                falling: [false; 2 * QUEENS],
                rising: [false; 2 * QUEENS],
            };
            place(&mut board, 0)
        }
        "hash" => hash(&mut random),
        "bytes" => bytes(&mut random),
        "list" => list(&mut random),
        _ => {
            println!("unknown kernel '{}'", kernel);
            std::process::exit(2);
        }
    };
    println!("{}", result);
}
