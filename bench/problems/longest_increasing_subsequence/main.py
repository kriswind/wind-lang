# Longest increasing subsequence; see main.wind. This version is the
# reference that the expected answers come from.
import sys

n = 3000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 5000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


values = [random() % 1000000 for _ in range(n)]
tails = []
for x in values:
    low = 0
    high = len(tails)
    while low < high:
        middle = (low + high) // 2
        if tails[middle] < x:
            low = middle + 1
        else:
            high = middle
    if low == len(tails):
        tails.append(x)
    else:
        tails[low] = x
print("length %d" % len(tails))
print("tails %d" % sum(tails))
