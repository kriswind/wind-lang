# The benchmark program in Python; see bench.wind for what each kernel measures.

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

seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


class Node:
    __slots__ = ("value", "next")

    def __init__(self):
        self.value = 0
        self.next = None


class Particle:
    __slots__ = ("x", "y", "vx", "vy")


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def sieve():
    composite = [False] * (SIEVE_LIMIT + 1)
    count = 0
    for i in range(2, SIEVE_LIMIT + 1):
        if composite[i]:
            continue
        count += 1
        for j in range(i * 2, SIEVE_LIMIT + 1, i):
            composite[j] = True
    return count


def collatz():
    best = 0
    best_start = 0
    for start in range(1, 300000):
        n = start
        steps = 0
        while n != 1:
            if n % 2 == 0:
                n //= 2
            else:
                n = 3 * n + 1
            steps += 1
        if steps > best:
            best = steps
            best_start = start
    return best_start * 1000 + best


def gcd(a, b):
    x = a
    y = b
    while y != 0:
        x, y = y, x % y
    return x


def gcd_sum():
    total = 0
    for i in range(1, 1201):
        for j in range(1, 1201):
            total += gcd(i, j)
    return total


def mandelbrot():
    size = 600
    total = 0
    for py in range(size):
        for px in range(size):
            cx = float(px) * 3.0 / float(size) - 2.0
            cy = float(py) * 2.0 / float(size) - 1.0
            x = 0.0
            y = 0.0
            i = 0
            while i < 200 and x * x + y * y <= 4.0:
                nxt = x * x - y * y + cx
                y = 2.0 * x * y + cy
                x = nxt
                i += 1
            total += i
    return total


def matmul():
    n = MATRIX_SIZE
    left = [float(i % 7) + 0.5 for i in range(n * n)]
    right = [float(i % 5) - 1.5 for i in range(n * n)]
    product = [0.0] * (n * n)
    for i in range(n):
        for j in range(n):
            total = 0.0
            for k in range(n):
                total += left[i * n + k] * right[k * n + j]
            product[i * n + j] = total
    total = 0.0
    for i in range(n * n):
        total += product[i]
    return int(total)


def particle_sum():
    particles = []
    for _ in range(PARTICLE_COUNT):
        p = Particle()
        p.x = float(random() % 1000)
        p.y = float(random() % 1000)
        p.vx = float(random() % 200 - 100) / 8.0
        p.vy = float(random() % 200 - 100) / 8.0
        particles.append(p)
    for _ in range(20000):
        for p in particles:
            p.x += p.vx * 0.5
            p.y += p.vy * 0.5
            if p.x < 0.0 or p.x > 1000.0:
                p.vx = -p.vx
            if p.y < 0.0 or p.y > 1000.0:
                p.vy = -p.vy
    total = 0.0
    for p in particles:
        total += p.x + p.y
    return int(total)


def quicksort(values, low, high):
    if low >= high:
        return
    pivot = values[(low + high) // 2]
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
    numbers = [random() % 1000000 for _ in range(SORT_COUNT)]
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
    global seed
    table = [0] * HASH_SIZE
    for _ in range(HASH_KEYS):
        key = random()
        slot = key % HASH_SIZE
        while table[slot] != 0 and table[slot] != key:
            slot = (slot + 1) % HASH_SIZE
        table[slot] = key
    seed = 42
    found = 0
    probes = 0
    for i in range(HASH_KEYS):
        key = random() + i % 2
        slot = key % HASH_SIZE
        while table[slot] != 0 and table[slot] != key:
            slot = (slot + 1) % HASH_SIZE
            probes += 1
        if table[slot] == key:
            found += 1
    return found * 100 + probes


def letters():
    text = bytearray(BYTES_COUNT)
    for i in range(BYTES_COUNT):
        text[i] = 97 + random() % 26
    counts = [0] * 26
    code = 0
    for i in range(BYTES_COUNT):
        letter = text[i]
        counts[letter - 97] += 1
        code = (code * 31 + letter) % 1000000007
    pairs = 0
    for i in range(1, BYTES_COUNT):
        if text[i - 1] == 119 and text[i] == 100:
            pairs += 1
    return code + counts[4] + pairs * 7


def linked_list():
    order = list(range(LIST_COUNT))
    nodes = [Node() for _ in range(LIST_COUNT)]
    for i in range(LIST_COUNT):
        nodes[i].value = i % 100
    for i in range(LIST_COUNT - 1, 0, -1):
        j = random() % (i + 1)
        order[i], order[j] = order[j], order[i]
    for i in range(LIST_COUNT - 1):
        nodes[order[i]].next = nodes[order[i + 1]]

    total = 0
    for pass_number in range(20):
        at = nodes[order[0]]
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
