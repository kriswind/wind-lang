# Trapping rain water; see main.wind. This version is the reference that the
# expected answers come from.
import sys

n = 10000000 if len(sys.argv) > 1 and sys.argv[1] == "bench" else 1000
seed = 42


def random():
    global seed
    seed = seed * 48271 % 2147483647
    return seed


def trapped(heights):
    left = 0
    right = len(heights) - 1
    left_max = 0
    right_max = 0
    water = 0
    while left < right:
        if heights[left] < heights[right]:
            if heights[left] >= left_max:
                left_max = heights[left]
            else:
                water += left_max - heights[left]
            left += 1
        else:
            if heights[right] >= right_max:
                right_max = heights[right]
            else:
                water += right_max - heights[right]
            right -= 1
    return water


heights = [random() % 1000 for _ in range(n)]
print(trapped(heights))
