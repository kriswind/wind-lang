// Game of Life; see main.wind.
use std::env;

fn step(source: &[u8], target: &mut [u8], size: usize) {
    for row in 0..size {
        let up = (row + size - 1) % size * size;
        let middle = row * size;
        let down = (row + 1) % size * size;
        for column in 0..size {
            let left = (column + size - 1) % size;
            let right = (column + 1) % size;
            let count = source[up + left] + source[up + column] + source[up + right]
                + source[middle + left] + source[middle + right]
                + source[down + left] + source[down + column] + source[down + right];
            let alive = source[middle + column] == 1;
            target[middle + column] = if count == 3 || (alive && count == 2) { 1 } else { 0 };
        }
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let size: usize = if bench { 256 } else { 32 };
    let generations = if bench { 500 } else { 40 };
    let cells = size * size;
    let mut seed: i64 = 42;
    let mut first: Vec<u8> = (0..cells)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            if seed % 3 == 0 { 1 } else { 0 }
        })
        .collect();
    let mut second = vec![0u8; cells];
    for _ in (0..generations).step_by(2) {
        step(&first, &mut second, size);
        step(&second, &mut first, size);
    }
    let mut alive: i64 = 0;
    let mut checksum: i64 = 0;
    for &cell in &first {
        alive += cell as i64;
        checksum = (checksum * 31 + cell as i64) % 1000000007;
    }
    println!("{} alive, checksum {}", alive, checksum);
}
