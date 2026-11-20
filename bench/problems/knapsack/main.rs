// 0/1 knapsack; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 1000 } else { 60 };
    let capacity = n * 125;
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let mut weight = vec![0usize; n];
    let mut value = vec![0i64; n];
    for i in 0..n {
        weight[i] = (1 + random() % 1000) as usize;
        value[i] = 1 + random() % 1000;
    }
    let mut best = vec![0i64; capacity + 1];
    for i in 0..n {
        let w = weight[i];
        let v = value[i];
        for c in (w..=capacity).rev() {
            let taken = best[c - w] + v;
            if taken > best[c] {
                best[c] = taken;
            }
        }
    }
    println!("{}", best[capacity]);
}
