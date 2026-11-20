# Benchmark results

Written by `wind run bench/run.wind` on 2026-11-20; running it again replaces this file.
Measured on AMD Ryzen 5 5600H with Radeon Graphics, Linux x86_64. Each time is the best of up to 5 runs.

Time relative to C, as the geometric mean over the 12 kernels. Lower is better, and one block is one times C.

```
C                          1.00x  █
Wind, no bounds checks     1.00x  █
Wind                       1.01x  █
Zig                        1.07x  █▏
C++                        1.08x  █▏
Odin                       1.10x  █▏
Rust                       1.11x  █▏
Swift                      1.19x  █▎
Go                         1.35x  █▍
Haskell                    1.69x  █▊
JavaScript (Bun)           1.92x  █▉
Lua (LuaJIT)               2.03x  ██
Java                       2.13x  ██▏
JavaScript (Node.js)       2.41x  ██▍
Elixir                     13.0x  █████████████
Lua                        13.4x  █████████████▍
Python                     33.7x  █████████████████████████████████▊
```

| | Built with | Toolchain | Relative to C | All kernels |
| --- | --- | --- | ---: | ---: |
| C | `cc -O2` | GCC 16.2.1 | 1.00x | 0.80 s |
| Wind, no bounds checks | `wind build --no-bounds-check` | wind 0.2.0 | 1.00x | 0.80 s |
| **Wind** | `wind build` | wind 0.2.0 | **1.01x** | 0.80 s |
| Zig | `zig build-exe -O ReleaseFast` | Zig 0.17.1 | 1.07x | 0.88 s |
| C++ | `c++ -O2` | GCC 16.2.1 | 1.08x | 0.87 s |
| Odin | `odin build -o:aggressive -no-bounds-check` | Odin dev-2026-10 | 1.10x | 0.86 s |
| Rust | `rustc -C opt-level=3 -C panic=abort` | rustc 1.91.1 | 1.11x | 0.90 s |
| Swift | `swiftc -Ounchecked` | Swift 6.3 | 1.19x | 0.92 s |
| Go | `go build` | Go 1.27.4 | 1.35x | 1.05 s |
| Haskell | `ghc -O2` | GHC 9.6.7 | 1.69x | 1.30 s |
| JavaScript (Bun) | JavaScript on Bun | Bun 1.3.12 | 1.92x | 1.36 s |
| Lua (LuaJIT) | Lua on LuaJIT | LuaJIT 2.1.1794214635 | 2.03x | 1.55 s |
| Java | `javac`, run on the JVM | OpenJDK 17.0.21.0.3 | 2.13x | 1.38 s |
| JavaScript (Node.js) | JavaScript on Node.js | Node.js 25.8.2 | 2.41x | 1.66 s |
| Elixir | `elixirc`, run on the BEAM | Elixir 1.20.4 | 13.0x | 10.94 s |
| Lua | Lua, the reference interpreter | Lua 5.5.1 | 13.4x | 9.83 s |
| Python | CPython | CPython 3.14.10 | 33.7x | 25.05 s |

## Each kernel

As a multiple of C's time:

| | fib | sieve | collatz | gcd | mandelbrot | matmul | particles | sort | queens | hash | bytes | list |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| C | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Wind, no bounds checks | 1.07 | 0.98 | 1.05 | 1.02 | 0.99 | 1.00 | 0.99 | 1.00 | 1.00 | 0.97 | 1.00 | 0.98 |
| Wind | 1.08 | 1.01 | 1.03 | 1.02 | 1.01 | 0.99 | 1.01 | 1.04 | 0.98 | 1.01 | 1.02 | 0.97 |
| Zig | 2.11 | 0.98 | 0.85 | 1.02 | 0.99 | 1.87 | 1.02 | 0.97 | 0.78 | 0.99 | 0.85 | 1.10 |
| C++ | 1.08 | 1.10 | 1.19 | 1.03 | 1.01 | 1.04 | 1.26 | 0.98 | 1.00 | 1.07 | 1.02 | 1.14 |
| Odin | 1.80 | 1.02 | 0.90 | 1.04 | 1.01 | 1.90 | 1.01 | 1.00 | 0.98 | 1.09 | 0.87 | 1.00 |
| Rust | 1.78 | 1.09 | 0.86 | 0.99 | 1.00 | 1.93 | 1.01 | 0.97 | 1.00 | 1.08 | 0.93 | 1.17 |
| Swift | 1.86 | 1.05 | 0.92 | 1.04 | 1.03 | 2.03 | 1.20 | 1.00 | 1.07 | 1.39 | 1.03 | 1.13 |
| Go | 4.04 | 1.15 | 1.40 | 1.08 | 1.04 | 2.04 | 1.71 | 1.05 | 1.05 | 1.24 | 0.97 | 1.10 |
| Haskell | 5.29 | 1.34 | 1.63 | 1.27 | 1.50 | 2.21 | 1.97 | 1.55 | 1.37 | 1.69 | 1.31 | 1.22 |
| JavaScript (Bun) | 5.46 | 1.60 | 4.62 | 1.32 | 1.29 | 3.01 | 2.05 | 1.26 | 1.53 | 1.91 | 1.65 | 1.00 |
| Lua (LuaJIT) | 6.69 | 1.20 | 3.60 | 2.00 | 1.05 | 2.99 | 1.25 | 2.73 | 2.40 | 1.98 | 1.71 | 0.97 |
| Java | 2.86 | 1.57 | 2.58 | 2.02 | 2.50 | 3.57 | 2.80 | 1.47 | 1.99 | 2.74 | 1.70 | 1.09 |
| JavaScript (Node.js) | 8.35 | 1.75 | 3.73 | 1.88 | 1.51 | 4.34 | 3.30 | 1.57 | 2.10 | 2.22 | 2.22 | 1.05 |
| Elixir | 9.92 | 20.4 | 6.55 | 8.34 | 16.8 | 46.7 | 35.7 | 19.1 | 6.13 | 12.9 | 6.30 | 7.88 |
| Lua | 45.2 | 10.4 | 27.2 | 4.30 | 15.4 | 63.1 | 44.6 | 7.16 | 9.02 | 8.31 | 8.55 | 3.05 |
| Python | 92.4 | 10.4 | 61.8 | 9.36 | 53.2 | 125 | 109 | 29.2 | 18.1 | 28.8 | 29.5 | 12.0 |

C's own times, in seconds, to give the ratios a scale:

| fib | sieve | collatz | gcd | mandelbrot | matmul | particles | sort | queens | hash | bytes | list |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0.044 | 0.085 | 0.033 | 0.026 | 0.049 | 0.024 | 0.022 | 0.132 | 0.032 | 0.026 | 0.034 | 0.291 |
