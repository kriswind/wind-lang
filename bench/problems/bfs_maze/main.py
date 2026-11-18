# Breadth-first search through a maze; see main.wind. This version is the
# reference that the expected answers come from.
import sys

side = 2000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 100
n = side * side
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


wall = bytearray(n)
for i in range(n):
    wall[i] = 1 if random() % 100 < 30 else 0
# The corners and the cells beside them are kept open.
for cell in (0, 1, side, n - 1, n - 2, n - 1 - side):
    wall[cell] = 0
dist = [-1] * n
queue = [0] * n
head = 0
tail = 1
dist[0] = 0


def visit(cell, d):
    global tail
    if wall[cell] == 0 and dist[cell] == -1:
        dist[cell] = d
        queue[tail] = cell
        tail += 1


while head < tail:
    cell = queue[head]
    head += 1
    d = dist[cell] + 1
    row = cell // side
    col = cell % side
    if row > 0:
        visit(cell - side, d)
    if row < side - 1:
        visit(cell + side, d)
    if col > 0:
        visit(cell - 1, d)
    if col < side - 1:
        visit(cell + 1, d)
print("%d steps, %d cells reached" % (dist[n - 1], tail))
