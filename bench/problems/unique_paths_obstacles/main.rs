// Unique paths with obstacles; see main.wind.
use std::env;

const PRIME: i64 = 1000000007;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let size: usize = if bench { 5000 } else { 120 };
    let (rows, columns) = (size, size);
    let mut seed: i64 = 42;
    let mut paths = vec![0i64; columns];
    let mut obstacles = 0;
    for i in 0..rows {
        for j in 0..columns {
            seed = seed * 48271 % 2147483647;
            let corner = (i == 0 && j == 0) || (i == rows - 1 && j == columns - 1);
            if seed % 10 == 0 && !corner {
                obstacles += 1;
                paths[j] = 0;
            } else if i == 0 && j == 0 {
                paths[j] = 1;
            } else if j > 0 {
                let mut total = paths[j] + paths[j - 1];
                if total >= PRIME {
                    total -= PRIME;
                }
                paths[j] = total;
            }
        }
    }
    println!("paths {}", paths[columns - 1]);
    println!("obstacles {}", obstacles);
}
