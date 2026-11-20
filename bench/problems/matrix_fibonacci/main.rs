// Matrix Fibonacci; see main.wind.
use std::env;

const PRIME: i64 = 67108859;

#[derive(Clone, Copy)]
struct Matrix {
    a: i64,
    b: i64,
    c: i64,
    d: i64,
}

fn multiply(x: Matrix, y: Matrix) -> Matrix {
    Matrix {
        a: (x.a * y.a + x.b * y.c) % PRIME,
        b: (x.a * y.b + x.b * y.d) % PRIME,
        c: (x.c * y.a + x.d * y.c) % PRIME,
        d: (x.c * y.b + x.d * y.d) % PRIME,
    }
}

fn fibonacci(n: i64) -> i64 {
    let mut result = Matrix { a: 1, b: 0, c: 0, d: 1 };
    let mut base = Matrix { a: 1, b: 1, c: 1, d: 0 };
    let mut left = n;
    while left > 0 {
        if left & 1 == 1 {
            result = multiply(result, base);
        }
        base = multiply(base, base);
        left >>= 1;
    }
    result.b
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let queries = if bench { 1000000 } else { 1000 };
    let mut seed: i64 = 42;
    let mut total: i64 = 0;
    for _ in 0..queries {
        seed = seed * 48271 % 2147483647;
        total = (total + fibonacci(seed)) % PRIME;
    }
    println!("{}", total);
}
