// Maximum subarray; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 10000000 } else { 1000 };
    let mut seed: i64 = 42;
    let values: Vec<i64> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            seed % 2001 - 1000
        })
        .collect();
    let mut best = values[0];
    let mut best_start = 0;
    let mut best_end = 0;
    let mut sum = values[0];
    let mut start = 0;
    for i in 1..n {
        if sum <= 0 {
            sum = values[i];
            start = i;
        } else {
            sum += values[i];
        }
        if sum > best {
            best = sum;
            best_start = start;
            best_end = i;
        }
    }
    println!("{} from {} to {}", best, best_start, best_end);
}
