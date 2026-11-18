# Dijkstra on a grid; see main.wind. This version is the reference that the
# expected answers come from.
import sys

side = 1500 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 40
n = side * side
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


cost = [1 + random() % 9 for _ in range(n)]
dist = [1 << 62] * n
heap = [0] * (4 * n + 1)
size = 0


def push(key):
    global size
    at = size
    size += 1
    while at > 0:
        parent = (at - 1) // 2
        if heap[parent] <= key:
            break
        heap[at] = heap[parent]
        at = parent
    heap[at] = key


def pop():
    global size
    top = heap[0]
    size -= 1
    last = heap[size]
    at = 0
    while True:
        child = 2 * at + 1
        if child >= size:
            break
        if child + 1 < size and heap[child + 1] < heap[child]:
            child += 1
        if last <= heap[child]:
            break
        heap[at] = heap[child]
        at = child
    heap[at] = last
    return top


def relax(cell, d):
    if d < dist[cell]:
        dist[cell] = d
        push(d * n + cell)


dist[0] = cost[0]
push(cost[0] * n)
while size > 0:
    key = pop()
    d = key // n
    cell = key % n
    if d > dist[cell]:
        continue
    row = cell // side
    col = cell % side
    if row > 0:
        relax(cell - side, d + cost[cell - side])
    if row < side - 1:
        relax(cell + side, d + cost[cell + side])
    if col > 0:
        relax(cell - 1, d + cost[cell - 1])
    if col < side - 1:
        relax(cell + 1, d + cost[cell + 1])
print("the cheapest path costs %d; the costs to every cell sum to %d" % (dist[n - 1], sum(dist)))
