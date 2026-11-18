# Next permutation; see main.wind. This version is the reference that the
# expected answers come from.
import sys

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
n = 10 if bench else 7
rounds = 4 if bench else 2
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


items = [0] * n
weights = [0] * n


def next_permutation():
    i = n - 2
    while i >= 0 and items[i] >= items[i + 1]:
        i -= 1
    if i < 0:
        return False
    j = n - 1
    while items[j] <= items[i]:
        j -= 1
    items[i], items[j] = items[j], items[i]
    low = i + 1
    high = n - 1
    while low < high:
        items[low], items[high] = items[high], items[low]
        low += 1
        high -= 1
    return True


count = 0
total = 0
checksum = 0
for _ in range(rounds):
    for i in range(n):
        items[i] = i
        weights[i] = random() % 1000 + 1
    while True:
        value = 0
        for i in range(n):
            value += items[i] * weights[i]
        count += 1
        total += value
        checksum = (checksum * 31 + value) % 1000000007
        if not next_permutation():
            break
print("%d permutations, total %d, checksum %d" % (count, total, checksum))
