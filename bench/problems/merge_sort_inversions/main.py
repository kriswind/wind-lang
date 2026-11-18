# Merge sort inversions; see main.wind. This version is the reference that
# the expected answers come from.
import sys

n = 2000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


values = [random() % n for _ in range(n)]
scratch = [0] * n


def merge(low, middle, high):
    i = low
    j = middle
    k = low
    inversions = 0
    while i < middle and j < high:
        if values[j] < values[i]:
            scratch[k] = values[j]
            j += 1
            inversions += middle - i
        else:
            scratch[k] = values[i]
            i += 1
        k += 1
    while i < middle:
        scratch[k] = values[i]
        i += 1
        k += 1
    while j < high:
        scratch[k] = values[j]
        j += 1
        k += 1
    values[low:high] = scratch[low:high]
    return inversions


def sort():
    inversions = 0
    width = 1
    while width < n:
        for low in range(0, n - width, 2 * width):
            inversions += merge(low, low + width, min(low + 2 * width, n))
        width *= 2
    return inversions


inversions = sort()
checksum = 0
for v in values:
    checksum = (checksum * 31 + v) % 1000000007
print("%d inversions, checksum %d" % (inversions, checksum))
