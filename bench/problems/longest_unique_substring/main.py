# Longest unique substring; see main.wind. This version is the reference that
# the expected answers come from.
import sys

n = 20000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 10000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


text = bytes(97 + random() % 26 for _ in range(n))
last = [-1] * 26
start = 0
best = 0
best_start = 0
for i in range(n):
    c = text[i] - 97
    if last[c] >= start:
        start = last[c] + 1
    last[c] = i
    if i - start + 1 > best:
        best = i - start + 1
        best_start = start
print("%d letters from %d: %s" % (best, best_start, text[best_start:best_start + best].decode()))
