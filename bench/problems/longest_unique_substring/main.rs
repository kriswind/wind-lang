// Longest unique substring; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 20000000 } else { 10000 };
    let mut seed: i64 = 42;
    let text: Vec<u8> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            (97 + seed % 26) as u8
        })
        .collect();
    let mut last = [-1i64; 26];
    let mut start: i64 = 0;
    let mut best: i64 = 0;
    let mut best_start: i64 = 0;
    for i in 0..n {
        let c = (text[i] - 97) as usize;
        if last[c] >= start {
            start = last[c] + 1;
        }
        last[c] = i as i64;
        if i as i64 - start + 1 > best {
            best = i as i64 - start + 1;
            best_start = start;
        }
    }
    let from = best_start as usize;
    let run = std::str::from_utf8(&text[from..from + best as usize]).unwrap();
    println!("{} letters from {}: {}", best, best_start, run);
}
