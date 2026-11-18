# Benchmark results

Written by `python3 bench/run.py` on 2026-11-18; running it again replaces this file.
Measured on AMD Ryzen 5 5600H with Radeon Graphics, Linux x86_64. Each time is the best of up to 5 runs.

Time relative to C, as the geometric mean over the 12 kernels. Lower is better, and one block is one times C.

```
Wind, no bounds checks     1.00x  █
C                          1.00x  █
Wind                       1.01x  █
C++                        1.07x  █▏
Zig                        1.08x  █▏
Odin                       1.09x  █▏
Rust                       1.11x  █▏
Swift                      1.18x  █▏
Go                         1.35x  █▍
Haskell                    1.68x  █▋
JavaScript (Bun)           1.92x  █▉
Lua (LuaJIT)               2.02x  ██
Java                       2.13x  ██▏
JavaScript (Node.js)       2.40x  ██▍
Elixir                     12.9x  ████████████▉
Lua                        13.3x  █████████████▎
Python                     33.2x  █████████████████████████████████▏
```

| | Built with | Toolchain | Relative to C | All kernels |
| --- | --- | --- | ---: | ---: |
| Wind, no bounds checks | `wind build --no-bounds-check` | wind 0.2.0 | 1.00x | 0.81 s |
| C | `cc -O2` | GCC 16.2.1 | 1.00x | 0.83 s |
| **Wind** | `wind build` | wind 0.2.0 | **1.01x** | 0.84 s |
| C++ | `c++ -O2` | GCC 16.2.1 | 1.07x | 0.88 s |
| Zig | `zig build-exe -O ReleaseFast` | Zig 0.17.1 | 1.08x | 0.90 s |
| Odin | `odin build -o:aggressive -no-bounds-check` | Odin dev-2026-10 | 1.09x | 0.88 s |
| Rust | `rustc -C opt-level=3 -C panic=abort` | rustc 1.91.1 | 1.11x | 0.90 s |
| Swift | `swiftc -Ounchecked` | Swift 6.3 | 1.18x | 0.92 s |
| Go | `go build` | Go 1.27.4 | 1.35x | 1.06 s |
| Haskell | `ghc -O2` | GHC 9.6.7 | 1.68x | 1.29 s |
| JavaScript (Bun) | JavaScript on Bun | Bun 1.3.12 | 1.92x | 1.36 s |
| Lua (LuaJIT) | Lua on LuaJIT | LuaJIT 2.1.1794214635 | 2.02x | 1.56 s |
| Java | `javac`, run on the JVM | OpenJDK 17.0.21.0.3 | 2.13x | 1.38 s |
| JavaScript (Node.js) | JavaScript on Node.js | Node.js 25.8.2 | 2.40x | 1.66 s |
| Elixir | `elixirc`, run on the BEAM | Elixir 1.20.4 | 12.9x | 10.95 s |
| Lua | Lua, the reference interpreter | Lua 5.5.1 | 13.3x | 9.69 s |
| Python | CPython | CPython 3.14.10 | 33.2x | 24.69 s |

## Each kernel

As a multiple of C's time:

| | fib | sieve | collatz | gcd | mandelbrot | matmul | particles | sort | queens | hash | bytes | list |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Wind, no bounds checks | 1.04 | 1.00 | 1.07 | 0.98 | 1.00 | 1.01 | 0.97 | 1.00 | 0.98 | 1.00 | 1.00 | 0.94 |
| C | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Wind | 1.05 | 1.02 | 1.05 | 0.98 | 1.00 | 1.01 | 0.99 | 1.04 | 0.97 | 1.01 | 0.99 | 1.02 |
| C++ | 1.06 | 1.13 | 1.17 | 1.02 | 1.01 | 1.09 | 1.21 | 0.99 | 1.03 | 1.05 | 1.01 | 1.09 |
| Zig | 2.12 | 0.99 | 0.86 | 0.99 | 1.00 | 1.94 | 1.03 | 0.98 | 0.78 | 0.99 | 0.83 | 1.07 |
| Odin | 1.77 | 1.01 | 0.89 | 1.01 | 1.00 | 1.97 | 1.01 | 1.00 | 0.99 | 1.11 | 0.84 | 1.00 |
| Rust | 1.76 | 1.14 | 0.87 | 0.98 | 1.00 | 1.96 | 1.01 | 0.97 | 1.01 | 1.10 | 0.93 | 1.04 |
| Swift | 1.83 | 1.04 | 0.92 | 1.02 | 1.04 | 2.12 | 1.19 | 1.00 | 1.05 | 1.45 | 1.04 | 1.01 |
| Go | 3.96 | 1.15 | 1.42 | 1.09 | 1.04 | 2.08 | 1.68 | 1.05 | 1.05 | 1.26 | 0.98 | 1.04 |
| Haskell | 4.82 | 1.35 | 1.64 | 1.28 | 1.52 | 2.27 | 1.95 | 1.55 | 1.37 | 1.70 | 1.30 | 1.14 |
| JavaScript (Bun) | 5.39 | 1.63 | 4.65 | 1.32 | 1.30 | 3.07 | 2.08 | 1.26 | 1.56 | 1.91 | 1.67 | 0.91 |
| Lua (LuaJIT) | 6.57 | 1.28 | 3.62 | 1.97 | 1.06 | 2.98 | 1.22 | 2.71 | 2.36 | 1.96 | 1.71 | 0.93 |
| Java | 2.86 | 1.59 | 2.59 | 2.07 | 2.51 | 3.62 | 2.77 | 1.45 | 2.03 | 2.82 | 1.74 | 1.00 |
| JavaScript (Node.js) | 8.21 | 1.72 | 3.78 | 1.93 | 1.53 | 4.51 | 3.17 | 1.59 | 2.17 | 2.24 | 2.22 | 0.97 |
| Elixir | 9.67 | 20.4 | 6.56 | 8.16 | 16.8 | 48.1 | 35.0 | 18.8 | 6.09 | 13.2 | 6.29 | 7.40 |
| Lua | 44.5 | 10.6 | 27.3 | 4.69 | 14.3 | 61.3 | 43.2 | 7.16 | 9.08 | 8.52 | 8.37 | 2.83 |
| Python | 91.3 | 10.3 | 59.9 | 9.32 | 52.4 | 128 | 102 | 28.7 | 18.3 | 29.0 | 29.7 | 10.9 |

C's own times, in seconds, to give the ratios a scale:

| fib | sieve | collatz | gcd | mandelbrot | matmul | particles | sort | queens | hash | bytes | list |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0.045 | 0.085 | 0.033 | 0.026 | 0.049 | 0.024 | 0.023 | 0.132 | 0.032 | 0.026 | 0.034 | 0.319 |
