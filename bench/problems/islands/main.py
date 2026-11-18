# Counting islands; see main.wind. This version is the reference that the
# expected answers come from.
import sys

side = 3000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 60
n = side * side
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


# 0 is water, 1 unvisited land, 2 land already counted.
land = bytearray(n)
for i in range(n):
    land[i] = 1 if random() % 100 < 45 else 0
stack = [0] * n
top = 0


def visit(cell):
    global top
    if land[cell] == 1:
        land[cell] = 2
        stack[top] = cell
        top += 1


islands = 0
largest = 0
for start in range(n):
    if land[start] != 1:
        continue
    islands += 1
    size = 0
    visit(start)
    while top > 0:
        top -= 1
        cell = stack[top]
        size += 1
        row = cell // side
        col = cell % side
        if row > 0:
            visit(cell - side)
        if row < side - 1:
            visit(cell + side)
        if col > 0:
            visit(cell - 1)
        if col < side - 1:
            visit(cell + 1)
    if size > largest:
        largest = size
print("%d islands, the largest of %d cells" % (islands, largest))
