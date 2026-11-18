// Edit distance; see main.wind.
use std::env;

const MAX: usize = 6000;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n = if bench { MAX } else { 300 };
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let a: Vec<u8> = (0..n).map(|_| (97 + random() % 4) as u8).collect();
    let b: Vec<u8> = (0..n).map(|_| (97 + random() % 4) as u8).collect();
    let mut previous: Vec<i64> = (0..=n as i64).collect();
    let mut current = vec![0i64; n + 1];
    for i in 1..=n {
        current[0] = i as i64;
        for j in 1..=n {
            let cost = if a[i - 1] == b[j - 1] { 0 } else { 1 };
            let mut best = previous[j - 1] + cost;
            if previous[j] + 1 < best {
                best = previous[j] + 1;
            }
            if current[j - 1] + 1 < best {
                best = current[j - 1] + 1;
            }
            current[j] = best;
        }
        std::mem::swap(&mut previous, &mut current);
    }
    println!("{}", previous[n]);
}
