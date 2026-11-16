#!/usr/bin/env python3
"""Builds the benchmark program in each language and times every kernel.

Run from the repository root:  python3 bench/run.py [options] [kernel...]

Each kernel is run as its own process, so a time includes starting the
program, and the best of several runs is kept. A language whose toolchain is
not installed is left out. Every language must print the same result for a
kernel; one that does not is reported and left out of the summary.
"""

import argparse
import math
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "build", "bench")

# What each kernel leans on, in the order they are shown.
KERNELS = [
    ("fib", "function calls"),
    ("sieve", "array writes"),
    ("collatz", "integer arithmetic"),
    ("gcd", "division in a called function"),
    ("mandelbrot", "float arithmetic"),
    ("matmul", "nested loops over arrays"),
    ("particles", "structs of floats"),
    ("sort", "recursion with swaps"),
    ("queens", "recursion with backtracking"),
    ("hash", "scattered reads and writes"),
    ("bytes", "bytes"),
    ("list", "pointer chasing"),
]
KERNEL_NAMES = [name for name, _ in KERNELS]

# A kernel that takes longer than this is not run again: its time is far from
# the noise that repeating is there to remove.
SLOW = 1.0


def source(name):
    return os.path.join(HERE, name)


def binary(name):
    return os.path.join(OUT, name)


def wind_compiler():
    local = os.path.join(ROOT, "build", "wind")
    return local if os.path.exists(local) else shutil.which("wind")


def elixir_command(compiled):
    """The command that runs the compiled Elixir program. It starts the
    Erlang VM itself, which is quicker than going through the elixir script."""
    if not shutil.which("elixir") or not shutil.which("erl"):
        return ["elixir", "-pa", compiled, "-e", "Bench.main(System.argv())"]
    library = subprocess.run(["elixir", "-e", "IO.write(:code.lib_dir(:elixir))"],
                             capture_output=True, text=True).stdout.strip()
    call = "'Elixir.Bench':main([list_to_binary(A) || A <- init:get_plain_arguments()]), halt()."
    return ["erl", "-noshell", "-noinput", "-pa", os.path.join(library, "ebin"), "-pa", compiled,
            "-eval", call, "-extra"]


def languages(cc, cxx):
    """Every language as a dict: its name, the tool it needs, how it is built
    (a command, or None) and run, and that same build spelled for people."""
    wind = wind_compiler()
    java_dir = binary("java")
    elixir_dir = binary("elixir")
    return [
        {"name": "wind", "tool": wind, "how": "`wind build`",
         "build": [wind, "build", source("bench.wind"), "-o", binary("wind")],
         "run": [binary("wind")]},
        {"name": "wind-unchecked", "tool": wind, "how": "`wind build --no-bounds-check`",
         "build": [wind, "build", "--no-bounds-check", source("bench.wind"), "-o", binary("wind-unchecked")],
         "run": [binary("wind-unchecked")]},
        {"name": "c", "tool": cc, "how": "`cc -O2`",
         "build": [cc, "-std=c11", "-O2", "-o", binary("c"), source("bench.c")],
         "run": [binary("c")]},
        {"name": "c++", "tool": cxx, "how": "`c++ -O2`",
         "build": [cxx, "-std=c++17", "-O2", "-o", binary("cpp"), source("bench.cpp")],
         "run": [binary("cpp")]},
        {"name": "rust", "tool": "rustc", "how": "`rustc -C opt-level=3 -C panic=abort`",
         "build": ["rustc", "-C", "opt-level=3", "-C", "panic=abort", "-o", binary("rust"), source("bench.rs")],
         "run": [binary("rust")]},
        {"name": "zig", "tool": "zig", "how": "`zig build-exe -O ReleaseFast`",
         "build": ["zig", "build-exe", "-O", "ReleaseFast", "--cache-dir", binary("zig-cache"),
                   "-femit-bin=" + binary("zig"), source("bench.zig")],
         "run": [binary("zig")]},
        {"name": "odin", "tool": "odin", "how": "`odin build -o:aggressive -no-bounds-check`",
         "build": ["odin", "build", source("bench.odin"), "-file", "-o:aggressive", "-no-bounds-check",
                   "-microarch:native", "-out:" + binary("odin")],
         "run": [binary("odin")]},
        {"name": "go", "tool": "go", "how": "`go build`",
         "build": ["go", "build", "-o", binary("go"), source("bench.go")],
         "run": [binary("go")]},
        {"name": "swift", "tool": "swiftc", "how": "`swiftc -Ounchecked`",
         "build": ["swiftc", "-Ounchecked", "-o", binary("swift"), source("bench.swift")],
         "run": [binary("swift")]},
        {"name": "haskell", "tool": "ghc", "how": "`ghc -O2`",
         "build": ["ghc", "-O2", "-dynamic", "-outputdir", binary("haskell-objects"),
                   "-o", binary("haskell"), source("bench.hs")],
         "run": [binary("haskell")]},
        {"name": "java", "tool": "javac", "how": "`javac`, run on the JVM",
         "build": ["javac", "-d", java_dir, source("Bench.java")],
         "run": ["java", "-cp", java_dir, "Bench"]},
        {"name": "node", "tool": "node", "how": "JavaScript on Node.js",
         "build": None, "run": ["node", source("bench.js")]},
        {"name": "bun", "tool": "bun", "how": "JavaScript on Bun",
         "build": None, "run": ["bun", source("bench.js")]},
        {"name": "elixir", "tool": "elixirc", "how": "`elixirc`, run on the BEAM",
         "build": ["elixirc", "-o", elixir_dir, source("bench.ex")],
         "run": elixir_command(elixir_dir)},
        {"name": "luajit", "tool": "luajit", "how": "Lua on LuaJIT",
         "build": None, "run": ["luajit", source("bench.lua")]},
        {"name": "lua", "tool": "lua", "how": "Lua, the reference interpreter",
         "build": None, "run": ["lua", source("bench.lua")]},
        {"name": "python", "tool": sys.executable, "how": "CPython",
         "build": None, "run": [sys.executable, source("bench.py")]},
    ]


def best_time(command, kernel, runs):
    """Returns the shortest of up to `runs` runs in seconds, and what the
    program printed; None and the problem if it fails."""
    best = None
    output = None
    for _ in range(runs):
        start = time.perf_counter()
        done = subprocess.run(command + [kernel], capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        if done.returncode != 0:
            return None, (done.stdout + done.stderr).strip() or "exit %d" % done.returncode
        output = done.stdout.strip()
        if best is None or elapsed < best:
            best = elapsed
        if elapsed > SLOW:
            break
    return best, output


def geometric_mean(values):
    return math.exp(sum(math.log(value) for value in values) / len(values))


def ratio_text(ratio):
    return "%.2f" % ratio if ratio < 10 else "%.1f" % ratio if ratio < 100 else "%.0f" % ratio


def print_table(rows, kernels, baseline, seconds):
    """rows: (name, {kernel: seconds}, mean ratio, total seconds), fastest first."""
    width = 8
    print("%-15s" % "" + "".join("%*s" % (width, kernel[: width - 1]) for kernel in kernels)
          + "%*s%*s" % (width, "mean", width, "total s"))
    for name, times, mean, total in rows:
        cells = []
        for kernel in kernels:
            if kernel not in times:
                cells.append("%*s" % (width, "-"))
            elif seconds:
                cells.append("%*.3f" % (width, times[kernel]))
            else:
                cells.append("%*s" % (width, ratio_text(times[kernel] / baseline[kernel])))
        print("%-15s" % name + "".join(cells)
              + "%*s%*s" % (width, ratio_text(mean) if mean else "-", width, "%.2f" % total))


def print_markdown(rows, kernels, baseline, hows):
    print("| | Built with | Time relative to C | All kernels |")
    print("| --- | --- | --- | --- |")
    for name, times, mean, total in rows:
        print("| %s | %s | %s | %.2f s |" % (name, hows[name], ratio_text(mean) + "x" if mean else "-", total))
    print()
    print("| | " + " | ".join(kernels) + " |")
    print("| --- |" + " ---: |" * len(kernels))
    for name, times, mean, total in rows:
        cells = [ratio_text(times[k] / baseline[k]) if k in times else "-" for k in kernels]
        print("| %s | %s |" % (name, " | ".join(cells)))
    print()
    print("| | " + " | ".join(kernels) + " |")
    print("| --- |" + " ---: |" * len(kernels))
    for name, times, mean, total in rows:
        cells = ["%.3f" % times[k] if k in times else "-" for k in kernels]
        print("| %s | %s |" % (name, " | ".join(cells)))


def main():
    parser = argparse.ArgumentParser(
        description="Time the benchmark kernels in each language.",
        epilog="kernels: " + ", ".join("%s (%s)" % kernel for kernel in KERNELS))
    parser.add_argument("kernels", nargs="*", metavar="kernel", help="kernels to run (default: all)")
    parser.add_argument("--runs", type=int, default=5,
                        help="runs per kernel, of which the best is kept; a kernel that takes over "
                             "%g s is run once (default: 5)" % SLOW)
    parser.add_argument("--only", action="append", default=[], metavar="language",
                        help="run only this language; may be repeated")
    parser.add_argument("--skip", action="append", default=[], metavar="language",
                        help="leave a language out; may be repeated")
    parser.add_argument("--seconds", action="store_true", help="show seconds in place of ratios")
    parser.add_argument("--markdown", action="store_true", help="print the results as Markdown tables")
    parser.add_argument("--cc", default=os.environ.get("CC") or "cc", help="the C compiler (default: $CC or cc)")
    parser.add_argument("--cxx", default=os.environ.get("CXX") or "c++",
                        help="the C++ compiler (default: $CXX or c++)")
    args = parser.parse_args()

    kernels = args.kernels or KERNEL_NAMES
    unknown = [k for k in kernels if k not in KERNEL_NAMES]
    if unknown:
        parser.error("unknown kernel: %s (choose from %s)" % (", ".join(unknown), ", ".join(KERNEL_NAMES)))

    known = languages(args.cc, args.cxx)
    names = [language["name"] for language in known]
    for name in args.only + args.skip:
        if name not in names:
            parser.error("unknown language: %s (choose from %s)" % (name, ", ".join(names)))

    os.makedirs(OUT, exist_ok=True)
    env = dict(os.environ, CC=args.cc)
    ready = []
    for language in known:
        name = language["name"]
        if name in args.skip or (args.only and name not in args.only):
            continue
        if not language["tool"] or not shutil.which(language["tool"]):
            continue
        if language["build"]:
            done = subprocess.run(language["build"], capture_output=True, text=True, env=env, cwd=HERE)
            if done.returncode != 0:
                print("%s: build failed\n%s" % (name, (done.stdout + done.stderr).strip()), file=sys.stderr)
                continue
        ready.append(language)
    if not ready:
        sys.exit("nothing to run")

    names = [language["name"] for language in ready]
    reference = "c" if "c" in names else names[0]
    times = {name: {} for name in names}
    wrong = False
    for kernel in kernels:
        outputs = {}
        for language in ready:
            name = language["name"]
            print("\r%-40s" % ("%s: %s" % (kernel, name)), end="", file=sys.stderr, flush=True)
            elapsed, outputs[name] = best_time(language["run"], kernel, args.runs)
            if elapsed is not None:
                times[name][kernel] = elapsed
        print("\r%-40s\r" % "", end="", file=sys.stderr, flush=True)
        expected = outputs[reference]
        for name in names:
            if kernel not in times[name]:
                print("%s %s: failed: %s" % (name, kernel, outputs[name]), file=sys.stderr)
            elif outputs[name] != expected:
                print("%s %s: printed %s, but %s printed %s"
                      % (name, kernel, outputs[name], reference, expected), file=sys.stderr)
                del times[name][kernel]
                wrong = True

    baseline = times[reference]
    rows = []
    for name in names:
        shared = [k for k in kernels if k in times[name] and k in baseline]
        mean = geometric_mean([times[name][k] / baseline[k] for k in shared]) if shared else None
        rows.append((name, times[name], mean, sum(times[name].values())))
    rows.sort(key=lambda row: row[2] if row[2] else float("inf"))

    if args.markdown:
        print_markdown(rows, kernels, baseline, {language["name"]: language["how"] for language in ready})
    else:
        what = "seconds" if args.seconds else "time relative to %s" % reference
        print("%s, best of up to %d run%s; mean is the geometric mean of the ratios to %s\n"
              % (what, args.runs, "" if args.runs == 1 else "s", reference))
        print_table(rows, kernels, baseline, args.seconds)
    sys.exit(1 if wrong else 0)


if __name__ == "__main__":
    main()
