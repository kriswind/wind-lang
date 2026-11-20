// Union-find; see main.wind.
use std::env;

struct Sets {
    parent: Vec<usize>,
    size: Vec<usize>,
}

impl Sets {
    fn find(&mut self, node: usize) -> usize {
        let mut at = node;
        while self.parent[at] != at {
            self.parent[at] = self.parent[self.parent[at]];
            at = self.parent[at];
        }
        at
    }

    fn join(&mut self, a: usize, b: usize) -> bool {
        let mut x = self.find(a);
        let mut y = self.find(b);
        if x == y {
            return false;
        }
        if self.size[x] < self.size[y] {
            std::mem::swap(&mut x, &mut y);
        }
        self.parent[y] = x;
        self.size[x] += self.size[y];
        true
    }
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 2000000 } else { 1000 };
    let mut seed: i64 = 42;
    let mut sets = Sets { parent: (0..n).collect(), size: vec![1; n] };
    let mut groups = n;
    for _ in 0..n * 9 / 10 {
        seed = seed * 48271 % 2147483647;
        let a = (seed % n as i64) as usize;
        seed = seed * 48271 % 2147483647;
        let b = (seed % n as i64) as usize;
        if sets.join(a, b) {
            groups -= 1;
        }
    }
    let mut largest = 0;
    for i in 0..n {
        if sets.parent[i] == i && sets.size[i] > largest {
            largest = sets.size[i];
        }
    }
    println!("{} groups, the largest of {}", groups, largest);
}
