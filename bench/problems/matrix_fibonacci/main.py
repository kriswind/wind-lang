# Matrix Fibonacci; see main.wind. This version is the reference that the
# expected answers come from.
import sys

PRIME = 67108859
queries = 1000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


def multiply(x, y):
    xa, xb, xc, xd = x
    ya, yb, yc, yd = y
    return ((xa * ya + xb * yc) % PRIME, (xa * yb + xb * yd) % PRIME,
            (xc * ya + xd * yc) % PRIME, (xc * yb + xd * yd) % PRIME)


def fibonacci(n):
    result = (1, 0, 0, 1)
    base = (1, 1, 1, 0)
    while n > 0:
        if n & 1 == 1:
            result = multiply(result, base)
        base = multiply(base, base)
        n >>= 1
    return result[1]


total = 0
for _ in range(queries):
    total = (total + fibonacci(random())) % PRIME
print(total)
