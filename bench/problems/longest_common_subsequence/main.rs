// Longest common subsequence; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 8000 } else { 400 };
    let m = n * 3 / 4;
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let a: Vec<u8> = (0..n).map(|_| (97 + random() % 8) as u8).collect();
    let b: Vec<u8> = (0..m).map(|_| (97 + random() % 8) as u8).collect();
    let mut previous = vec![0i64; m + 1];
    let mut current = vec![0i64; m + 1];
    for i in 1..=n {
        let letter = a[i - 1];
        for j in 1..=m {
            current[j] = if letter == b[j - 1] {
                previous[j - 1] + 1
            } else if previous[j] > current[j - 1] {
                previous[j]
            } else {
                current[j - 1]
            };
        }
        std::mem::swap(&mut previous, &mut current);
    }
    println!("{}", previous[m]);
}
