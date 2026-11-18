# Sliding window maximum; see main.wind. This version is the reference that
# the expected answers come from.
import sys

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
n = 10000000 if bench else 1000
k = 1000 if bench else 16
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


values = [random() % 1000000 for _ in range(n)]
deque = [0] * n
head = 0
tail = 0
total = 0
for i in range(n):
    while tail > head and values[deque[tail - 1]] <= values[i]:
        tail -= 1
    deque[tail] = i
    tail += 1
    if deque[head] <= i - k:
        head += 1
    if i >= k - 1:
        total += values[deque[head]]
print("%d windows of %d, the maxima summing to %d" % (n - k + 1, k, total))
