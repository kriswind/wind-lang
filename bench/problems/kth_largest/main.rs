// Kth largest; see main.wind.
use std::env;

fn push(heap: &mut Vec<i64>, value: i64) {
    let mut at = heap.len();
    heap.push(value);
    while at > 0 {
        let parent = (at - 1) / 2;
        if heap[parent] <= heap[at] {
            break;
        }
        heap.swap(parent, at);
        at = parent;
    }
}

fn replace_root(heap: &mut [i64], value: i64) {
    heap[0] = value;
    let size = heap.len();
    let mut at = 0;
    loop {
        let mut child = 2 * at + 1;
        if child >= size {
            break;
        }
        if child + 1 < size && heap[child + 1] < heap[child] {
            child += 1;
        }
        if heap[at] <= heap[child] {
            break;
        }
        heap.swap(child, at);
        at = child;
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n = if bench { 30000000 } else { 10000 };
    let k = if bench { 100000 } else { 100 };
    let mut seed: i64 = 42;
    let mut heap: Vec<i64> = Vec::with_capacity(k);
    for _ in 0..n {
        seed = seed * 48271 % 2147483647;
        let value = seed;
        if heap.len() < k {
            push(&mut heap, value);
        } else if value > heap[0] {
            replace_root(&mut heap, value);
        }
    }
    let sum: i64 = heap.iter().sum();
    println!("sum {}, k-th largest {}", sum, heap[0]);
}
