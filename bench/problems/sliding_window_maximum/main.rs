// Sliding window maximum; see main.wind.
use std::env;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 10000000 } else { 1000 };
    let k: usize = if bench { 1000 } else { 16 };
    let mut seed: i64 = 42;
    let values: Vec<i64> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            seed % 1000000
        })
        .collect();
    let mut deque = vec![0usize; n];
    let mut head = 0;
    let mut tail = 0;
    let mut sum: i64 = 0;
    for i in 0..n {
        while tail > head && values[deque[tail - 1]] <= values[i] {
            tail -= 1;
        }
        deque[tail] = i;
        tail += 1;
        if deque[head] + k <= i {
            head += 1;
        }
        if i + 1 >= k {
            sum += values[deque[head]];
        }
    }
    println!("{} windows of {}, the maxima summing to {}", n - k + 1, k, sum);
}
