// Merge sort inversions; see main.wind.
use std::env;

fn merge(values: &mut [i64], scratch: &mut [i64], low: usize, middle: usize, high: usize) -> i64 {
    let (mut i, mut j, mut k) = (low, middle, low);
    let mut inversions = 0;
    while i < middle && j < high {
        if values[j] < values[i] {
            scratch[k] = values[j];
            j += 1;
            inversions += (middle - i) as i64;
        } else {
            scratch[k] = values[i];
            i += 1;
        }
        k += 1;
    }
    while i < middle {
        scratch[k] = values[i];
        i += 1;
        k += 1;
    }
    while j < high {
        scratch[k] = values[j];
        j += 1;
        k += 1;
    }
    values[low..high].copy_from_slice(&scratch[low..high]);
    inversions
}

fn sort(values: &mut [i64], scratch: &mut [i64]) -> i64 {
    let n = values.len();
    let mut inversions = 0;
    let mut width = 1;
    while width < n {
        let mut low = 0;
        while low < n - width {
            let high = if low + 2 * width < n { low + 2 * width } else { n };
            inversions += merge(values, scratch, low, low + width, high);
            low += 2 * width;
        }
        width *= 2;
    }
    inversions
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 2000000 } else { 1000 };
    let mut seed: i64 = 42;
    let mut values: Vec<i64> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            seed % n as i64
        })
        .collect();
    let mut scratch = vec![0i64; n];
    let inversions = sort(&mut values, &mut scratch);
    let mut checksum: i64 = 0;
    for &v in &values {
        checksum = (checksum * 31 + v) % 1000000007;
    }
    println!("{} inversions, checksum {}", inversions, checksum);
}
