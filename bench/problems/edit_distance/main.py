# Edit distance; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 6000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 300
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


a = [97 + random() % 4 for _ in range(n)]
b = [97 + random() % 4 for _ in range(n)]
previous = list(range(n + 1))
for i in range(1, n + 1):
    current = [i] + [0] * n
    ai = a[i - 1]
    for j in range(1, n + 1):
        best = previous[j - 1] + (0 if ai == b[j - 1] else 1)
        if previous[j] + 1 < best:
            best = previous[j] + 1
        if current[j - 1] + 1 < best:
            best = current[j - 1] + 1
        current[j] = best
    previous = current
print(previous[n])
