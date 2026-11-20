// Topological sort; see main.wind.
use std::env;

const MAX: usize = 1000000;

struct Heap {
    nodes: Vec<usize>,
    size: usize,
}

impl Heap {
    fn push(&mut self, node: usize) {
        let mut at = self.size;
        self.size += 1;
        while at > 0 {
            let parent = (at - 1) / 2;
            if self.nodes[parent] <= node {
                break;
            }
            self.nodes[at] = self.nodes[parent];
            at = parent;
        }
        self.nodes[at] = node;
    }

    fn pop(&mut self) -> usize {
        let top = self.nodes[0];
        self.size -= 1;
        let last = self.nodes[self.size];
        let mut at = 0;
        loop {
            let mut child = 2 * at + 1;
            if child >= self.size {
                break;
            }
            if child + 1 < self.size && self.nodes[child + 1] < self.nodes[child] {
                child += 1;
            }
            if last <= self.nodes[child] {
                break;
            }
            self.nodes[at] = self.nodes[child];
            at = child;
        }
        self.nodes[at] = last;
        top
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n = if bench { MAX } else { 1000 };
    let m = 4 * n;
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let mut label: Vec<usize> = (0..n).collect();
    for i in (1..n).rev() {
        let j = (random() % (i as i64 + 1)) as usize;
        label.swap(i, j);
    }
    let mut source = vec![0usize; m];
    let mut target = vec![0usize; m];
    let mut edges = 0;
    for _ in 0..m {
        let mut a = (random() % n as i64) as usize;
        let mut b = (random() % n as i64) as usize;
        if a == b {
            continue;
        }
        if a > b {
            std::mem::swap(&mut a, &mut b);
        }
        source[edges] = label[a];
        target[edges] = label[b];
        edges += 1;
    }
    let mut first = vec![0usize; n + 1];
    let mut waiting = vec![0usize; n];
    for e in 0..edges {
        first[source[e] + 1] += 1;
        waiting[target[e]] += 1;
    }
    for i in 0..n {
        first[i + 1] += first[i];
    }
    let mut fill: Vec<usize> = first[..n].to_vec();
    let mut after = vec![0usize; edges];
    for e in 0..edges {
        after[fill[source[e]]] = target[e];
        fill[source[e]] += 1;
    }

    let mut heap = Heap { nodes: vec![0; n], size: 0 };
    for i in 0..n {
        if waiting[i] == 0 {
            heap.push(i);
        }
    }
    let mut placed = 0;
    let mut checksum: i64 = 0;
    while heap.size > 0 {
        let node = heap.pop();
        placed += 1;
        checksum = (checksum * 31 + node as i64) % 1000000007;
        for k in first[node]..first[node + 1] {
            let v = after[k];
            waiting[v] -= 1;
            if waiting[v] == 0 {
                heap.push(v);
            }
        }
    }
    println!("{} edges, {} nodes placed, checksum {}", edges, placed, checksum);
}
