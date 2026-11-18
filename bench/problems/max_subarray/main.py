# Maximum subarray; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 10000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


values = [random() % 2001 - 1000 for _ in range(n)]
best = values[0]
best_start = 0
best_end = 0
total = values[0]
start = 0
for i in range(1, n):
    if total <= 0:
        total = values[i]
        start = i
    else:
        total += values[i]
    if total > best:
        best = total
        best_start = start
        best_end = i
print("%d from %d to %d" % (best, best_start, best_end))
