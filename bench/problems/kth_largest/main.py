# Kth largest; see main.wind. This version is the reference that the
# expected answers come from.
import sys

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
n = 30000000 if bench else 10000
k = 100000 if bench else 100
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


heap = []


def push(value):
    at = len(heap)
    heap.append(value)
    while at > 0:
        parent = (at - 1) // 2
        if heap[parent] <= heap[at]:
            break
        heap[parent], heap[at] = heap[at], heap[parent]
        at = parent


def replace_root(value):
    heap[0] = value
    size = len(heap)
    at = 0
    while True:
        child = 2 * at + 1
        if child >= size:
            break
        if child + 1 < size and heap[child + 1] < heap[child]:
            child += 1
        if heap[at] <= heap[child]:
            break
        heap[child], heap[at] = heap[at], heap[child]
        at = child


for _ in range(n):
    value = random()
    if len(heap) < k:
        push(value)
    elif value > heap[0]:
        replace_root(value)
print("sum %d, k-th largest %d" % (sum(heap), heap[0]))
