# Two sum; see main.wind. This version is the reference that the expected
# answers come from.
import sys

n = 2000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


size = 1
while size < 2 * n:
    size *= 2
mask = size - 1
keys = [-1] * size
counts = [0] * size


def slot(key):
    """The slot holding key, or the empty slot (key -1) where it would go."""
    at = (key * 2654435761) & mask
    while keys[at] != -1 and keys[at] != key:
        at = (at + 1) & mask
    return at


values = [random() % n for _ in range(n)]
pairs = 0
distinct = 0
for value in values:
    want = n - value
    at = slot(want)
    if keys[at] == want:
        pairs += counts[at]
    at = slot(value)
    if keys[at] != value:
        keys[at] = value
        distinct += 1
    counts[at] += 1
print("%d pairs, %d distinct values" % (pairs, distinct))
