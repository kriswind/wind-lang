// Longest increasing subsequence; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 3000000 } else { 5000 };
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let values: Vec<i64> = (0..n).map(|_| random() % 1000000).collect();
    let mut tails = vec![0i64; n];
    let mut length = 0;
    for &x in &values {
        let mut low = 0;
        let mut high = length;
        while low < high {
            let middle = (low + high) / 2;
            if tails[middle] < x {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        tails[low] = x;
        if low == length {
            length += 1;
        }
    }
    let sum: i64 = tails[..length].iter().sum();
    println!("length {}", length);
    println!("tails {}", sum);
}
