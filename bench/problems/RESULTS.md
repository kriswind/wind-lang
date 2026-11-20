# Problem set results

Written by `wind run bench/problems/run.wind --size bench` on 2026-11-18; running it again replaces this file.
Measured on AMD Ryzen 5 5600H with Radeon Graphics, Linux x86_64. Each time is the best of up to 3 runs.

Time at bench size as a multiple of C's, and C's own time in seconds. Every program's answer was checked first.

| Problem | Wind | Wind, no bounds checks | Rust | Go | JavaScript (Node.js) | Python | C, seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `bfs_maze` | 1.18 | 0.96 | 1.02 | 1.33 | 3.95 | 56.6 | 0.061 |
| `coin_change` | 1.27 | 0.94 | 1.49 | 1.08 | 4.94 | 265 | 0.047 |
| `dijkstra_grid` | 1.00 | 1.00 | 1.03 | 1.13 | 2.17 | 47.7 | 0.147 |
| `edit_distance` | 1.01 | 1.01 | 1.67 | 2.59 | 4.32 | 256 | 0.049 |
| `game_of_life` | 0.99 | 0.99 | 1.21 | 1.10 | 1.62 | 84.0 | 0.134 |
| `islands` | 1.23 | 0.98 | 0.99 | 1.64 | 6.11 | 69.9 | 0.074 |
| `knapsack` | 1.29 | 0.98 | 2.18 | 1.19 | 2.42 | 339 | 0.062 |
| `kth_largest` | 0.99 | 1.01 | 1.09 | 1.09 | 5.10 | 78.3 | 0.132 |
| `longest_common_subsequence` | 1.02 | 1.00 | 1.06 | 1.91 | 3.12 | 148 | 0.068 |
| `longest_increasing_subsequence` | 0.88 | 1.00 | 0.88 | 1.00 | 1.51 | 48.5 | 0.146 |
| `longest_unique_substring` | 0.92 | 0.91 | 0.87 | 1.21 | 4.74 | 73.2 | 0.103 |
| `matrix_fibonacci` | 1.00 | 0.99 | 1.37 | 1.11 | 12.6 | 108 | 0.171 |
| `max_subarray` | 1.02 | 1.02 | 1.19 | 1.10 | 5.31 | 74.5 | 0.045 |
| `merge_sort_inversions` | 1.01 | 0.99 | 1.04 | 1.21 | 2.52 | 45.6 | 0.168 |
| `next_permutation` | 1.04 | 1.05 | 0.93 | 2.03 | 4.95 | 292 | 0.080 |
| `sliding_window_maximum` | 1.02 | 1.01 | 1.01 | 1.02 | 3.63 | 71.2 | 0.105 |
| `sudoku` | 0.98 | 1.00 | 1.19 | 1.26 | 1.51 | 62.8 | 0.170 |
| `topological_sort` | 1.03 | 0.99 | 1.08 | 1.20 | 1.77 | 47.5 | 0.273 |
| `trapping_rain_water` | 0.86 | 0.86 | 0.96 | 1.02 | 9.26 | 59.0 | 0.048 |
| `two_sum` | 1.15 | 0.98 | 1.05 | 1.12 | 2.88 | 36.3 | 0.062 |
| `union_find` | 1.56 | 0.96 | 1.11 | 1.56 | 2.42 | 30.8 | 0.083 |
| `unique_paths_obstacles` | 0.99 | 0.99 | 0.73 | 0.70 | 3.69 | 67.1 | 0.152 |
| **mean** | **1.06** | **0.98** | **1.11** | **1.25** | **3.50** | **82.7** | |

The mean is the geometric mean of the ratios.

| | Toolchain |
| --- | --- |
| Wind | wind 0.2.0 |
| Wind, no bounds checks | wind 0.2.0 |
| C | GCC 16.2.1 |
| Rust | rustc 1.91.1 |
| Go | Go 1.27.4 |
| JavaScript (Node.js) | Node.js 25.8.2 |
| Python | CPython 3.14.10 |
