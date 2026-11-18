// Breadth-first search through a maze; see main.wind.
use std::env;

const MAX_SIDE: usize = 2000;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let side = if bench { MAX_SIDE } else { 100 };
    let n = side * side;
    let mut seed: i64 = 42;
    let mut wall = vec![0u8; n];
    for w in wall.iter_mut() {
        seed = seed * 48271 % 2147483647;
        *w = if seed % 100 < 30 { 1 } else { 0 };
    }
    for cell in [0, 1, side, n - 1, n - 2, n - 1 - side] {
        wall[cell] = 0;
    }
    let mut dist = vec![-1i64; n];
    let mut queue = vec![0usize; n];
    let mut head = 0;
    let mut tail = 1;
    dist[0] = 0;
    while head < tail {
        let cell = queue[head];
        head += 1;
        let d = dist[cell] + 1;
        let row = cell / side;
        let col = cell % side;
        let mut visit = |next: usize| {
            if wall[next] == 0 && dist[next] == -1 {
                dist[next] = d;
                queue[tail] = next;
                tail += 1;
            }
        };
        if row > 0 {
            visit(cell - side);
        }
        if row < side - 1 {
            visit(cell + side);
        }
        if col > 0 {
            visit(cell - 1);
        }
        if col < side - 1 {
            visit(cell + 1);
        }
    }
    println!("{} steps, {} cells reached", dist[n - 1], tail);
}
