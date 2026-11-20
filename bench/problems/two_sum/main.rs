// Two sum; see main.wind.
use std::env;

struct Table {
    keys: Vec<i64>,
    counts: Vec<i64>,
    mask: usize,
}

impl Table {
    // The slot holding key, or the empty slot (key -1) where it would go.
    fn slot(&self, key: i64) -> usize {
        let mut at = (key * 2654435761) as usize & self.mask;
        while self.keys[at] != -1 && self.keys[at] != key {
            at = (at + 1) & self.mask;
        }
        at
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: i64 = if bench { 2000000 } else { 1000 };
    let mut seed: i64 = 42;
    let mut size: usize = 1;
    while (size as i64) < 2 * n {
        size *= 2;
    }
    let mut table = Table { keys: vec![-1; size], counts: vec![0; size], mask: size - 1 };
    let values: Vec<i64> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            seed % n
        })
        .collect();
    let mut pairs: i64 = 0;
    let mut distinct: i64 = 0;
    for &value in &values {
        let want = n - value;
        let mut at = table.slot(want);
        if table.keys[at] == want {
            pairs += table.counts[at];
        }
        at = table.slot(value);
        if table.keys[at] != value {
            table.keys[at] = value;
            distinct += 1;
        }
        table.counts[at] += 1;
    }
    println!("{} pairs, {} distinct values", pairs, distinct);
}
