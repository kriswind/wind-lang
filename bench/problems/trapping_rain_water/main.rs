// Trapping rain water; see main.wind.
use std::env;

fn trapped(heights: &[i64]) -> i64 {
    let mut left = 0;
    let mut right = heights.len() - 1;
    let mut left_max = 0;
    let mut right_max = 0;
    let mut water = 0;
    while left < right {
        if heights[left] < heights[right] {
            if heights[left] >= left_max {
                left_max = heights[left];
            } else {
                water += left_max - heights[left];
            }
            left += 1;
        } else {
            if heights[right] >= right_max {
                right_max = heights[right];
            } else {
                water += right_max - heights[right];
            }
            right -= 1;
        }
    }
    water
}

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let n: usize = if bench { 10000000 } else { 1000 };
    let mut seed: i64 = 42;
    let heights: Vec<i64> = (0..n)
        .map(|_| {
            seed = seed * 48271 % 2147483647;
            seed % 1000
        })
        .collect();
    println!("{}", trapped(&heights));
}
