# Union-find; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 2000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


parent = list(range(n))
size = [1] * n


def find(node):
    at = node
    while parent[at] != at:
        parent[at] = parent[parent[at]]
        at = parent[at]
    return at


def join(a, b):
    x = find(a)
    y = find(b)
    if x == y:
        return False
    if size[x] < size[y]:
        x, y = y, x
    parent[y] = x
    size[x] += size[y]
    return True


groups = n
for _ in range(n * 9 // 10):
    a = random() % n
    b = random() % n
    if join(a, b):
        groups -= 1
largest = max(size[i] for i in range(n) if parent[i] == i)
print("%d groups, the largest of %d" % (groups, largest))
