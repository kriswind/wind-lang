// Next permutation; see main.wind.
use std::env;

fn next_permutation(items: &mut [i64]) -> bool {
    let n = items.len();
    let mut i = n as isize - 2;
    while i >= 0 && items[i as usize] >= items[i as usize + 1] {
        i -= 1;
    }
    if i < 0 {
        return false;
    }
    let i = i as usize;
    let mut j = n - 1;
    while items[j] <= items[i] {
        j -= 1;
    }
    items.swap(i, j);
    let (mut low, mut high) = (i + 1, n - 1);
    while low < high {
        items.swap(low, high);
        low += 1;
        high -= 1;
    }
    true
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n = if bench { 10 } else { 7 };
    let rounds = if bench { 4 } else { 2 };
    let mut seed: i64 = 42;
    let mut items = vec![0i64; n];
    let mut weights = vec![0i64; n];
    let (mut count, mut total, mut checksum) = (0i64, 0i64, 0i64);
    for _ in 0..rounds {
        for i in 0..n {
            items[i] = i as i64;
            seed = seed * 48271 % 2147483647;
            weights[i] = seed % 1000 + 1;
        }
        loop {
            let mut value = 0;
            for i in 0..n {
                value += items[i] * weights[i];
            }
            count += 1;
            total += value;
            checksum = (checksum * 31 + value) % 1000000007;
            if !next_permutation(&mut items) {
                break;
            }
        }
    }
    println!("{} permutations, total {}, checksum {}", count, total, checksum);
}
