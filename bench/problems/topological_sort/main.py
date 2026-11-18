# Topological sort; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 1000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
m = 4 * n
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


label = list(range(n))
for i in range(n - 1, 0, -1):
    j = random() % (i + 1)
    label[i], label[j] = label[j], label[i]

source = [0] * m
target = [0] * m
edges = 0
for _ in range(m):
    a = random() % n
    b = random() % n
    if a == b:
        continue
    if a > b:
        a, b = b, a
    source[edges] = label[a]
    target[edges] = label[b]
    edges += 1

first = [0] * (n + 1)
waiting = [0] * n
for e in range(edges):
    first[source[e] + 1] += 1
    waiting[target[e]] += 1
for i in range(n):
    first[i + 1] += first[i]
fill = first[:n]
after = [0] * edges
for e in range(edges):
    after[fill[source[e]]] = target[e]
    fill[source[e]] += 1

heap = [0] * n
size = 0


def push(node):
    global size
    at = size
    size += 1
    while at > 0:
        parent = (at - 1) // 2
        if heap[parent] <= node:
            break
        heap[at] = heap[parent]
        at = parent
    heap[at] = node


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


for i in range(n):
    if waiting[i] == 0:
        push(i)
placed = 0
checksum = 0
while size > 0:
    node = pop()
    placed += 1
    checksum = (checksum * 31 + node) % 1000000007
    for k in range(first[node], first[node + 1]):
        v = after[k]
        waiting[v] -= 1
        if waiting[v] == 0:
            push(v)
print("%d edges, %d nodes placed, checksum %d" % (edges, placed, checksum))
