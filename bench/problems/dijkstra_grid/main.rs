// Dijkstra on a grid; see main.wind.
use std::env;

const MAX_SIDE: usize = 1500;

struct Heap {
    keys: Vec<i64>,
    size: usize,
}

impl Heap {
    fn push(&mut self, key: i64) {
        let mut at = self.size;
        self.size += 1;
        while at > 0 {
            let parent = (at - 1) / 2;
            if self.keys[parent] <= key {
                break;
            }
            self.keys[at] = self.keys[parent];
            at = parent;
        }
        self.keys[at] = key;
    }

    fn pop(&mut self) -> i64 {
        let top = self.keys[0];
        self.size -= 1;
        let last = self.keys[self.size];
        let mut at = 0;
        loop {
            let mut child = 2 * at + 1;
            if child >= self.size {
                break;
            }
            if child + 1 < self.size && self.keys[child + 1] < self.keys[child] {
                child += 1;
            }
            if last <= self.keys[child] {
                break;
            }
            self.keys[at] = self.keys[child];
            at = child;
        }
        self.keys[at] = last;
        top
    }
}

fn relax(dist: &mut [i64], heap: &mut Heap, n: usize, cell: usize, d: i64) {
    if d < dist[cell] {
        dist[cell] = d;
        heap.push(d * n as i64 + cell as i64);
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let side = if bench { MAX_SIDE } else { 40 };
    let n = side * side;
    let mut seed: i64 = 42;
    let mut cost = vec![0i64; n];
    for c in cost.iter_mut() {
        seed = seed * 48271 % 2147483647;
        *c = 1 + seed % 9;
    }
    let mut dist = vec![1i64 << 62; n];
    let mut heap = Heap { keys: vec![0; 4 * n + 1], size: 0 };
    dist[0] = cost[0];
    heap.push(cost[0] * n as i64);
    while heap.size > 0 {
        let key = heap.pop();
        let d = key / n as i64;
        let cell = (key % n as i64) as usize;
        if d > dist[cell] {
            continue;
        }
        let row = cell / side;
        let col = cell % side;
        if row > 0 {
            relax(&mut dist, &mut heap, n, cell - side, d + cost[cell - side]);
        }
        if row < side - 1 {
            relax(&mut dist, &mut heap, n, cell + side, d + cost[cell + side]);
        }
        if col > 0 {
            relax(&mut dist, &mut heap, n, cell - 1, d + cost[cell - 1]);
        }
        if col < side - 1 {
            relax(&mut dist, &mut heap, n, cell + 1, d + cost[cell + 1]);
        }
    }
    let total: i64 = dist.iter().sum();
    println!("the cheapest path costs {}; the costs to every cell sum to {}", dist[n - 1], total);
}
