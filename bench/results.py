"""What the benchmark scripts share for writing their results files: the
Markdown files in bench/ that the README links to, which each full run of a
script writes again."""

import datetime
import math
import os
import platform
import re
import subprocess

VERSION = re.compile(r"dev-\d{4}-\d{2}|\d+(?:\.\d+)+")


def tool_version(command, pick=None):
    """The version a toolchain reports for itself: the first version number
    in what the command prints, on the first line that holds pick if given.
    None if the command cannot be run or prints no version."""
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in (done.stdout + done.stderr).splitlines():
        if pick and pick not in line:
            continue
        found = VERSION.search(line)
        if found:
            return found.group(0)
    return None


def machine():
    """The processor and system the benchmarks ran on, for the reader to
    weigh the numbers by."""
    cpu = None
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    cpu = cpu or platform.processor() or "an unknown processor"
    return "%s, %s %s" % (cpu, platform.system(), platform.machine())


def geometric_mean(values):
    return math.exp(sum(math.log(value) for value in values) / len(values))


def ratio_text(ratio):
    return "%.2f" % ratio if ratio < 10 else "%.1f" % ratio if ratio < 100 else "%.0f" % ratio


def bar(ratio):
    """A bar one block long per times the baseline, in eighths of a block."""
    eighths = max(1, round(ratio * 8))
    partial = ("", "▏", "▎", "▍", "▌", "▋", "▊", "▉")
    return "█" * (eighths // 8) + partial[eighths % 8]


def heading(title, command, timing):
    """The top of a results file: what it is, what wrote it, where it ran,
    and how each time was taken, as in "the best of 3 runs"."""
    return ("# %s\n\n"
            "Written by `%s` on %s; running it again replaces this file.\n"
            "Measured on %s. Each time is %s."
            % (title, command, datetime.date.today().isoformat(), machine(), timing))


def write(path, text):
    with open(path, "w") as f:
        f.write(text)
    print("wrote %s" % os.path.relpath(path))
