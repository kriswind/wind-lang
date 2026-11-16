# The benchmark program in Python; see bench.wind for what each kernel measures.
# It is written for speed in plain CPython: names are kept local, loops run
# over the values themselves where they can, the sieve marks a whole run of
# multiples with one slice assignment, bytes are searched with the methods a
# bytearray has, and the random numbers a kernel needs are made in one go.

import sys

SIEVE_LIMIT = 20000000
SORT_COUNT = 2000000
MATRIX_SIZE = 400
LIST_COUNT = 1000000
HASH_SIZE = 2097152
HASH_KEYS = 1000000
BYTES_COUNT = 4000000
PARTICLE_COUNT = 1000
QUEENS = 12

sys.setrecursionlimit(10000)


def randoms(count, seed=42):
    """The next `count` numbers of the generator that starts from seed."""
    numbers = [0] * count
    for i in range(count):
        seed = seed * 48271 % 2147483647
        numbers[i] = seed
    return numbers


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def sieve():
    limit = SIEVE_LIMIT
    composite = bytearray(limit + 1)
    count = 0
    for i in range(2, limit + 1):
        if composite[i]:
            continue
        count += 1
        if i * 2 <= limit:
            composite[i * 2::i] = b"\x01" * ((limit - i * 2) // i + 1)
    return count


def collatz():
    best = 0
    best_start = 0
    for start in range(1, 300000):
        n = start
        steps = 0
        while n != 1:
            if n & 1:
                n = 3 * n + 1
            else:
                n >>= 1
            steps += 1
        if steps > best:
            best = steps
            best_start = start
    return best_start * 1000 + best


def gcd_sum():
    total = 0
    for i in range(1, 1201):
        for j in range(1, 1201):
            x = i
            y = j
            while y:
                x, y = y, x % y
            total += x
    return total


def mandelbrot():
    size = 600
    total = 0
    for py in range(size):
        cy = py * 2.0 / size - 1.0
        for px in range(size):
            cx = px * 3.0 / size - 2.0
            x = 0.0
            y = 0.0
            i = 0
            while i < 200 and x * x + y * y <= 4.0:
                x, y = x * x - y * y + cx, 2.0 * x * y + cy
                i += 1
            total += i
    return total


def matmul():
    n = MATRIX_SIZE
    left = [i % 7 + 0.5 for i in range(n * n)]
    right = [i % 5 - 1.5 for i in range(n * n)]
    rows = [left[i * n:(i + 1) * n] for i in range(n)]
    columns = [right[j::n] for j in range(n)]
    total = 0.0
    for row in rows:
        for column in columns:
            cell = 0.0
            for a, b in zip(row, column):
                cell += a * b
            total += cell
    return int(total)


def particle_sum():
    numbers = randoms(PARTICLE_COUNT * 4)
    xs = [float(r % 1000) for r in numbers[0::4]]
    ys = [float(r % 1000) for r in numbers[1::4]]
    vxs = [(r % 200 - 100) / 8.0 for r in numbers[2::4]]
    vys = [(r % 200 - 100) / 8.0 for r in numbers[3::4]]
    places = range(PARTICLE_COUNT)
    for _ in range(20000):
        for i in places:
            x = xs[i] + vxs[i] * 0.5
            y = ys[i] + vys[i] * 0.5
            xs[i] = x
            ys[i] = y
            if x < 0.0 or x > 1000.0:
                vxs[i] = -vxs[i]
            if y < 0.0 or y > 1000.0:
                vys[i] = -vys[i]
    total = 0.0
    for x, y in zip(xs, ys):
        total += x + y
    return int(total)


def quicksort(values, low, high):
    if low >= high:
        return
    pivot = values[(low + high) >> 1]
    i = low
    j = high
    while i <= j:
        while values[i] < pivot:
            i += 1
        while values[j] > pivot:
            j -= 1
        if i <= j:
            values[i], values[j] = values[j], values[i]
            i += 1
            j -= 1
    quicksort(values, low, j)
    quicksort(values, i, high)


def sort():
    numbers = [r % 1000000 for r in randoms(SORT_COUNT)]
    quicksort(numbers, 0, SORT_COUNT - 1)
    check = 0
    for i in range(0, SORT_COUNT, 1000):
        check += numbers[i] * (i % 7 + 1)
    return check


columns = [False] * QUEENS
falling = [False] * (2 * QUEENS)
rising = [False] * (2 * QUEENS)


def place(row):
    if row == QUEENS:
        return 1
    ways = 0
    for column in range(QUEENS):
        down = row + column
        up = row - column + QUEENS
        if columns[column] or falling[down] or rising[up]:
            continue
        columns[column] = True
        falling[down] = True
        rising[up] = True
        ways += place(row + 1)
        columns[column] = False
        falling[down] = False
        rising[up] = False
    return ways


def hash_table():
    mask = HASH_SIZE - 1
    table = [0] * HASH_SIZE
    keys = randoms(HASH_KEYS)
    for key in keys:
        slot = key & mask
        held = table[slot]
        while held and held != key:
            slot = (slot + 1) & mask
            held = table[slot]
        table[slot] = key
    found = 0
    probes = 0
    odd = 0
    for key in keys:
        key += odd
        odd ^= 1
        slot = key & mask
        held = table[slot]
        while held and held != key:
            slot = (slot + 1) & mask
            probes += 1
            held = table[slot]
        if held == key:
            found += 1
    return found * 100 + probes


def letters():
    text = bytearray(97 + r % 26 for r in randoms(BYTES_COUNT))
    code = 0
    for letter in text:
        code = (code * 31 + letter) % 1000000007
    return code + text.count(b"e") + text.count(b"wd") * 7


class Node:
    __slots__ = ("value", "next")

    def __init__(self, value):
        self.value = value
        self.next = None


def linked_list():
    order = list(range(LIST_COUNT))
    nodes = [Node(i % 100) for i in range(LIST_COUNT)]
    numbers = randoms(LIST_COUNT - 1)
    at = 0
    for i in range(LIST_COUNT - 1, 0, -1):
        j = numbers[at] % (i + 1)
        at += 1
        order[i], order[j] = order[j], order[i]
    for i in range(LIST_COUNT - 1):
        nodes[order[i]].next = nodes[order[i + 1]]

    total = 0
    first = nodes[order[0]]
    for pass_number in range(20):
        at = first
        while at is not None:
            total += at.value + pass_number
            at = at.next
    return total


KERNELS = {
    "fib": lambda: fib(38),
    "sieve": sieve,
    "collatz": collatz,
    "gcd": gcd_sum,
    "mandelbrot": mandelbrot,
    "matmul": matmul,
    "particles": particle_sum,
    "sort": sort,
    "queens": lambda: place(0),
    "hash": hash_table,
    "bytes": letters,
    "list": linked_list,
}

kernel = sys.argv[1] if len(sys.argv) > 1 else ""
if kernel not in KERNELS:
    print("unknown kernel '%s'" % kernel)
    sys.exit(2)
print(KERNELS[kernel]())
