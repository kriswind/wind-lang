# Unique paths with obstacles; see main.wind. This version is the reference
# that the expected answers come from.
import sys

size = 5000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 120
rows = columns = size
PRIME = 1000000007
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


paths = [0] * columns
obstacles = 0
for i in range(rows):
    for j in range(columns):
        corner = (i == 0 and j == 0) or (i == rows - 1 and j == columns - 1)
        if random() % 10 == 0 and not corner:
            obstacles += 1
            paths[j] = 0
        elif i == 0 and j == 0:
            paths[j] = 1
        elif j > 0:
            paths[j] = (paths[j] + paths[j - 1]) % PRIME
print("paths %d" % paths[columns - 1])
print("obstacles %d" % obstacles)
