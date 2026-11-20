// Counting islands; see main.wind.
use std::env;

const MAX_SIDE: usize = 3000;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let side = if bench { MAX_SIDE } else { 60 };
    let n = side * side;
    let mut seed: i64 = 42;
    // 0 is water, 1 unvisited land, 2 land already counted.
    let mut land = vec![0u8; n];
    for l in land.iter_mut() {
        seed = seed * 48271 % 2147483647;
        *l = if seed % 100 < 45 { 1 } else { 0 };
    }
    let mut stack = vec![0usize; n];
    let mut top = 0;
    let mut islands = 0;
    let mut largest = 0;
    for start in 0..n {
        if land[start] != 1 {
            continue;
        }
        islands += 1;
        let mut size = 0;
        land[start] = 2;
        stack[top] = start;
        top += 1;
        while top > 0 {
            top -= 1;
            let cell = stack[top];
            size += 1;
            let row = cell / side;
            let col = cell % side;
            let mut visit = |next: usize| {
                if land[next] == 1 {
                    land[next] = 2;
                    stack[top] = next;
                    top += 1;
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
        if size > largest {
            largest = size;
        }
    }
    println!("{} islands, the largest of {} cells", islands, largest);
}
