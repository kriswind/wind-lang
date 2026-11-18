# Problem set results

Written by `python3 bench/problems/run.py --size bench` on 2026-11-18; running it again replaces this file.
Measured on AMD Ryzen 5 5600H with Radeon Graphics, Linux x86_64. Each time is the best of up to 3 runs.

Time at bench size as a multiple of C's, and C's own time in seconds. Every program's answer was checked first.

| Problem | Wind | Wind, no bounds checks | Rust | Go | JavaScript (Node.js) | Python | C, seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `bfs_maze` | 1.14 | 0.96 | 0.97 | 1.31 | 3.84 | 55.7 | 0.062 |
| `coin_change` | 1.29 | 1.05 | 1.52 | 1.14 | 5.18 | 282 | 0.045 |
| `dijkstra_grid` | 1.00 | 1.01 | 1.03 | 1.15 | 2.18 | 48.2 | 0.147 |
| `edit_distance` | 1.00 | 1.01 | 1.72 | 2.59 | 4.41 | 249 | 0.049 |
| `game_of_life` | 0.98 | 0.98 | 1.20 | 1.12 | 1.61 | 82.7 | 0.135 |
| `islands` | 1.22 | 0.97 | 0.94 | 1.65 | 6.17 | 72.6 | 0.074 |
| `knapsack` | 1.31 | 0.97 | 2.16 | 1.18 | 2.44 | 327 | 0.063 |
| `kth_largest` | 0.99 | 0.98 | 1.07 | 1.07 | 5.12 | 76.5 | 0.134 |
| `longest_common_subsequence` | 0.99 | 0.99 | 0.95 | 1.85 | 3.11 | 153 | 0.070 |
| `longest_increasing_subsequence` | 0.88 | 1.00 | 0.91 | 1.02 | 1.56 | 47.8 | 0.145 |
| `longest_unique_substring` | 0.94 | 0.91 | 0.89 | 1.24 | 4.78 | 71.8 | 0.102 |
| `matrix_fibonacci` | 1.02 | 1.00 | 1.39 | 1.15 | 13.0 | 111 | 0.168 |
| `max_subarray` | 0.99 | 0.98 | 1.17 | 1.07 | 5.26 | 73.6 | 0.046 |
| `merge_sort_inversions` | 1.01 | 0.98 | 1.03 | 1.23 | 2.50 | 45.8 | 0.167 |
| `next_permutation` | 1.06 | 1.07 | 0.97 | 2.05 | 5.05 | 287 | 0.079 |
| `sliding_window_maximum` | 1.02 | 1.01 | 1.05 | 1.03 | 3.62 | 71.9 | 0.105 |
| `sudoku` | 0.98 | 1.00 | 1.16 | 1.26 | 1.51 | 62.8 | 0.170 |
| `topological_sort` | 1.06 | 1.02 | 1.12 | 1.24 | 1.86 | 48.2 | 0.266 |
| `trapping_rain_water` | 0.89 | 0.87 | 1.01 | 1.04 | 9.58 | 59.1 | 0.047 |
| `two_sum` | 1.14 | 0.96 | 1.06 | 1.13 | 2.84 | 36.9 | 0.063 |
| `union_find` | 1.25 | 0.98 | 1.12 | 1.52 | 2.45 | 29.9 | 0.081 |
| `unique_paths_obstacles` | 0.99 | 0.99 | 0.73 | 0.70 | 3.67 | 67.9 | 0.152 |
| **mean** | **1.05** | **0.98** | **1.11** | **1.26** | **3.54** | **82.7** | |

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
