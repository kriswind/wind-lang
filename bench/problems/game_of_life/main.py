# Game of Life; see main.wind. This version is the reference that the
# expected answers come from.
import sys

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
size = 256 if bench else 32
generations = 500 if bench else 40
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


def step(source, target):
    for row in range(size):
        up = (row + size - 1) % size * size
        middle = row * size
        down = (row + 1) % size * size
        for column in range(size):
            left = (column + size - 1) % size
            right = (column + 1) % size
            count = (source[up + left] + source[up + column] + source[up + right]
                     + source[middle + left] + source[middle + right]
                     + source[down + left] + source[down + column] + source[down + right])
            alive = source[middle + column] == 1
            target[middle + column] = 1 if count == 3 or (alive and count == 2) else 0


cells = size * size
first = [1 if random() % 3 == 0 else 0 for _ in range(cells)]
second = [0] * cells
for _ in range(0, generations, 2):
    step(first, second)
    step(second, first)
checksum = 0
for cell in first:
    checksum = (checksum * 31 + cell) % 1000000007
print("%d alive, checksum %d" % (sum(first), checksum))
