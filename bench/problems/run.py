#!/usr/bin/env python3
"""Builds, checks and times the problem set in bench/problems.

Run from the repository root:

    python3 bench/problems/run.py                    # check every problem at test size
    python3 bench/problems/run.py --size bench       # check and time at bench size
    python3 bench/problems/run.py --only wind        # one language
    python3 bench/problems/run.py --skip python edit_distance union_find
    python3 bench/problems/run.py --expected         # write the expected answers

Each problem is a folder holding the same program in several languages
(main.wind, main.c, main.rs, main.go, main.js, main.py), which takes "test"
or "bench" as its argument, generates its input with the shared generator
(seed = seed * 48271 % 2147483647, from 42) and prints its answer. Every
program's output must equal the folder's expected-test.txt or
expected-bench.txt, which --expected writes from the Python version, the
reference. A language whose toolchain is missing is left out.

The exit status is 1 if any program fails to build, fails to run, or gives
a different answer.
"""

import argparse
import math
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "problems")
REFERENCE = "python"


def wind_compiler():
    local = os.path.join(ROOT, "build", "wind")
    return local if os.path.exists(local) else shutil.which("wind")


def languages():
    """Each language: the source file, the tool it needs, and how to build
    (None for an interpreted one) and run it, given the source and the path
    of the program to build."""
    wind = wind_compiler()
    return [
        {"name": "wind", "file": "main.wind", "tool": wind,
         "build": lambda src, exe: [wind, "build", src, "-o", exe], "run": lambda src, exe: [exe]},
        {"name": "wind-unchecked", "file": "main.wind", "tool": wind,
         "build": lambda src, exe: [wind, "build", "--no-bounds-check", src, "-o", exe],
         "run": lambda src, exe: [exe]},
        {"name": "c", "file": "main.c", "tool": "cc",
         "build": lambda src, exe: ["cc", "-std=c11", "-O2", "-o", exe, src, "-lm"],
         "run": lambda src, exe: [exe]},
        {"name": "rust", "file": "main.rs", "tool": "rustc",
         "build": lambda src, exe: ["rustc", "-C", "opt-level=3", "-C", "panic=abort", "-o", exe, src],
         "run": lambda src, exe: [exe]},
        {"name": "go", "file": "main.go", "tool": "go",
         "build": lambda src, exe: ["go", "build", "-o", exe, src], "run": lambda src, exe: [exe]},
        {"name": "js", "file": "main.js", "tool": "node",
         "build": None, "run": lambda src, exe: ["node", src]},
        {"name": "python", "file": "main.py", "tool": sys.executable,
         "build": None, "run": lambda src, exe: [sys.executable, src]},
    ]


def problems():
    return sorted(name for name in os.listdir(HERE)
                  if os.path.isdir(os.path.join(HERE, name)) and not name.startswith("."))


def run_once(command, size):
    start = time.perf_counter()
    done = subprocess.run(command + [size], capture_output=True, text=True)
    return time.perf_counter() - start, done


def main():
    parser = argparse.ArgumentParser(description="Build, check and time the problem set.")
    parser.add_argument("problems", nargs="*", help="problems to run (default: all)")
    parser.add_argument("--size", choices=["test", "bench"], default="test")
    parser.add_argument("--runs", type=int, default=3, help="runs per program at bench size (default: 3)")
    parser.add_argument("--only", action="append", default=[], metavar="language")
    parser.add_argument("--skip", action="append", default=[], metavar="language")
    parser.add_argument("--expected", action="store_true",
                        help="write expected-<size>.txt from the reference, then check the others")
    args = parser.parse_args()

    known = problems()
    chosen = args.problems or known
    for name in chosen:
        if name not in known:
            parser.error("unknown problem: %s (choose from %s)" % (name, ", ".join(known)))
    langs = [lang for lang in languages()
             if (not args.only or lang["name"] in args.only) and lang["name"] not in args.skip
             and lang["tool"] and shutil.which(lang["tool"])]
    if args.expected and REFERENCE not in [lang["name"] for lang in langs]:
        parser.error("--expected needs the reference language, %s" % REFERENCE)
    runs = args.runs if args.size == "bench" else 1

    failures = 0
    times = {}  # (problem, language) -> seconds
    for problem in chosen:
        folder = os.path.join(HERE, problem)
        expected_path = os.path.join(folder, "expected-%s.txt" % args.size)
        ordered = sorted(langs, key=lambda lang: lang["name"] != REFERENCE)
        expected = None
        if not args.expected and os.path.exists(expected_path):
            with open(expected_path) as f:
                expected = f.read()
        for lang in ordered:
            src = os.path.join(folder, lang["file"])
            if not os.path.exists(src):
                continue
            exe = os.path.join(OUT, problem, lang["name"])
            if lang["build"]:
                os.makedirs(os.path.dirname(exe), exist_ok=True)
                built = subprocess.run(lang["build"](src, exe), capture_output=True, text=True)
                if built.returncode != 0:
                    failures += 1
                    print("FAIL %s %s: build\n%s" % (problem, lang["name"],
                                                    (built.stdout + built.stderr).strip()))
                    continue
            best = None
            for _ in range(runs):
                elapsed, done = run_once(lang["run"](src, exe), args.size)
                if done.returncode != 0:
                    break
                best = elapsed if best is None else min(best, elapsed)
            if done.returncode != 0:
                failures += 1
                print("FAIL %s %s: exit %d\n%s" % (problem, lang["name"], done.returncode,
                                                  (done.stdout + done.stderr).strip()[-2000:]))
                continue
            if args.expected and lang["name"] == REFERENCE:
                with open(expected_path, "w") as f:
                    f.write(done.stdout)
                expected = done.stdout
            if expected is None:
                failures += 1
                print("FAIL %s: no %s; write it with --expected" % (problem, os.path.basename(expected_path)))
                break
            if done.stdout != expected:
                failures += 1
                print("FAIL %s %s: wrong answer\n--- expected\n%s--- printed\n%s" %
                      (problem, lang["name"], expected, done.stdout))
                continue
            times[(problem, lang["name"])] = best

    names = [lang["name"] for lang in langs]
    checked = len(times)
    print("%d checked, %d failed" % (checked, failures))
    if args.size == "bench" and times:
        base = "c" if "c" in names else names[0]
        print("\nseconds, best of %d; the mean is the geometric mean of the ratios to %s\n" % (runs, base))
        width = max(len(p) for p in chosen) + 2
        print("%-*s" % (width, "") + "".join("%16s" % n for n in names))
        ratios = {n: [] for n in names}
        for problem in chosen:
            cells = []
            for n in names:
                t = times.get((problem, n))
                cells.append("%16s" % ("-" if t is None else "%.3f" % t))
                if t is not None and times.get((problem, base)):
                    ratios[n].append(t / times[(problem, base)])
            print("%-*s" % (width, problem) + "".join(cells))
        means = []
        for n in names:
            r = ratios[n]
            means.append("%16s" % ("-" if not r else "%.2fx" % math.exp(sum(map(math.log, r)) / len(r))))
        print("%-*s" % (width, "mean") + "".join(means))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
