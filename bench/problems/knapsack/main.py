# 0/1 knapsack; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 1000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 60
capacity = n * 125
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


weight = []
value = []
for _ in range(n):
    weight.append(1 + random() % 1000)
    value.append(1 + random() % 1000)

best = [0] * (capacity + 1)
for i in range(n):
    w = weight[i]
    v = value[i]
    for c in range(capacity, w - 1, -1):
        taken = best[c - w] + v
        if taken > best[c]:
            best[c] = taken
print(best[capacity])
