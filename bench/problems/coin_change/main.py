# Coin change; see main.wind. This version is the reference that the
# expected answers come from.
import sys

bench = len(sys.argv) > 1 and sys.argv[1] == "bench"
count = 40 if bench else 8
limit = 1000000 if bench else 2000
PRIME = 1000000007
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


coins = []
for _ in range(count):
    coin = 2 + random() % 199
    while coin in coins:
        coin = 2 + random() % 199
    coins.append(coin)

ways = [1] + [0] * limit
for coin in coins:
    for a in range(coin, limit + 1):
        ways[a] = (ways[a] + ways[a - coin]) % PRIME

# An amount that cannot be made keeps the value limit + 1.
fewest = [0] + [limit + 1] * limit
for coin in coins:
    for a in range(coin, limit + 1):
        used = fewest[a - coin] + 1
        if used < fewest[a]:
            fewest[a] = used

ways_sum = sum(ways[1:]) % PRIME
fewest_sum = sum(f for f in fewest[1:] if f <= limit)
unreachable = sum(1 for f in fewest[1:] if f > limit)
print("ways %d" % ways_sum)
print("fewest %d" % fewest_sum)
print("unreachable %d" % unreachable)
