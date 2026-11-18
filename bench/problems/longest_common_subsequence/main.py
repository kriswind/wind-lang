# Longest common subsequence; see main.wind. This version is the reference
# that the expected answers come from.
import sys

n = 8000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 400
m = n * 3 // 4
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


a = [97 + random() % 8 for _ in range(n)]
b = [97 + random() % 8 for _ in range(m)]
previous = [0] * (m + 1)
current = [0] * (m + 1)
for i in range(1, n + 1):
    letter = a[i - 1]
    for j in range(1, m + 1):
        if letter == b[j - 1]:
            current[j] = previous[j - 1] + 1
        elif previous[j] > current[j - 1]:
            current[j] = previous[j]
        else:
            current[j] = current[j - 1]
    previous, current = current, previous
print(previous[m])
