-- The benchmark program in Lua; see bench.wind for what each kernel measures.
-- It keeps to what Lua 5.1 has, so that LuaJIT runs the same file, and its
-- tables are indexed from 0 as the arrays of the other languages are.

local floor = math.floor

local SIEVE_LIMIT = 20000000
local SORT_COUNT = 2000000
local MATRIX_SIZE = 400
local LIST_COUNT = 1000000
local HASH_SIZE = 2097152
local HASH_KEYS = 1000000
local BYTES_COUNT = 4000000
local PARTICLE_COUNT = 1000
local QUEENS = 12

local seed = 42

local function random()
    seed = seed * 48271 % 2147483647
    return seed
end

-- Drops the fraction, towards zero, as a conversion to an integer does.
local function truncate(value)
    if value < 0 then return -floor(-value) end
    return floor(value)
end

local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end

local function sieve()
    local composite = {}
    for i = 0, SIEVE_LIMIT do composite[i] = false end
    local count = 0
    for i = 2, SIEVE_LIMIT do
        if not composite[i] then
            count = count + 1
            for j = i * 2, SIEVE_LIMIT, i do composite[j] = true end
        end
    end
    return count
end

local function collatz()
    local best = 0
    local best_start = 0
    for start = 1, 299999 do
        local n = start
        local steps = 0
        while n ~= 1 do
            if n % 2 == 0 then n = floor(n / 2) else n = 3 * n + 1 end
            steps = steps + 1
        end
        if steps > best then
            best = steps
            best_start = start
        end
    end
    return best_start * 1000 + best
end

local function gcd(a, b)
    local x = a
    local y = b
    while y ~= 0 do
        local rest = x % y
        x = y
        y = rest
    end
    return x
end

local function gcd_sum()
    local total = 0
    for i = 1, 1200 do
        for j = 1, 1200 do total = total + gcd(i, j) end
    end
    return total
end

local function mandelbrot()
    local size = 600
    local total = 0
    for py = 0, size - 1 do
        for px = 0, size - 1 do
            local cx = px * 3.0 / size - 2.0
            local cy = py * 2.0 / size - 1.0
            local x = 0.0
            local y = 0.0
            local i = 0
            while i < 200 and x * x + y * y <= 4.0 do
                local next = x * x - y * y + cx
                y = 2.0 * x * y + cy
                x = next
                i = i + 1
            end
            total = total + i
        end
    end
    return total
end

local function matmul()
    local n = MATRIX_SIZE
    local left, right, product = {}, {}, {}
    for i = 0, n * n - 1 do
        left[i] = i % 7 + 0.5
        right[i] = i % 5 - 1.5
    end
    for i = 0, n - 1 do
        for j = 0, n - 1 do
            local sum = 0.0
            for k = 0, n - 1 do sum = sum + left[i * n + k] * right[k * n + j] end
            product[i * n + j] = sum
        end
    end
    local total = 0.0
    for i = 0, n * n - 1 do total = total + product[i] end
    return truncate(total)
end

local function particle_sum()
    local particles = {}
    for i = 0, PARTICLE_COUNT - 1 do
        local p = {}
        p.x = random() % 1000 + 0.0
        p.y = random() % 1000 + 0.0
        p.vx = (random() % 200 - 100) / 8.0
        p.vy = (random() % 200 - 100) / 8.0
        particles[i] = p
    end
    for _ = 1, 20000 do
        for i = 0, PARTICLE_COUNT - 1 do
            local p = particles[i]
            p.x = p.x + p.vx * 0.5
            p.y = p.y + p.vy * 0.5
            if p.x < 0.0 or p.x > 1000.0 then p.vx = -p.vx end
            if p.y < 0.0 or p.y > 1000.0 then p.vy = -p.vy end
        end
    end
    local total = 0.0
    for i = 0, PARTICLE_COUNT - 1 do total = total + particles[i].x + particles[i].y end
    return truncate(total)
end

local function quicksort(values, low, high)
    if low >= high then return end
    local pivot = values[floor((low + high) / 2)]
    local i = low
    local j = high
    while i <= j do
        while values[i] < pivot do i = i + 1 end
        while values[j] > pivot do j = j - 1 end
        if i <= j then
            values[i], values[j] = values[j], values[i]
            i = i + 1
            j = j - 1
        end
    end
    quicksort(values, low, j)
    quicksort(values, i, high)
end

local function sort()
    local numbers = {}
    for i = 0, SORT_COUNT - 1 do numbers[i] = random() % 1000000 end
    quicksort(numbers, 0, SORT_COUNT - 1)
    local check = 0
    for i = 0, SORT_COUNT - 1, 1000 do check = check + numbers[i] * (i % 7 + 1) end
    return check
end

local columns, falling, rising = {}, {}, {}
for i = 0, 2 * QUEENS do
    columns[i] = false
    falling[i] = false
    rising[i] = false
end

local function place(row)
    if row == QUEENS then return 1 end
    local ways = 0
    for column = 0, QUEENS - 1 do
        local down = row + column
        local up = row - column + QUEENS
        if not (columns[column] or falling[down] or rising[up]) then
            columns[column] = true
            falling[down] = true
            rising[up] = true
            ways = ways + place(row + 1)
            columns[column] = false
            falling[down] = false
            rising[up] = false
        end
    end
    return ways
end

local function hash()
    local table = {}
    for i = 0, HASH_SIZE - 1 do table[i] = 0 end
    for _ = 1, HASH_KEYS do
        local key = random()
        local slot = key % HASH_SIZE
        while table[slot] ~= 0 and table[slot] ~= key do slot = (slot + 1) % HASH_SIZE end
        table[slot] = key
    end
    seed = 42
    local found = 0
    local probes = 0
    for i = 0, HASH_KEYS - 1 do
        local key = random() + i % 2
        local slot = key % HASH_SIZE
        while table[slot] ~= 0 and table[slot] ~= key do
            slot = (slot + 1) % HASH_SIZE
            probes = probes + 1
        end
        if table[slot] == key then found = found + 1 end
    end
    return found * 100 + probes
end

local function bytes()
    local text = {}
    for i = 0, BYTES_COUNT - 1 do text[i] = 97 + random() % 26 end
    local counts = {}
    for i = 0, 25 do counts[i] = 0 end
    local code = 0
    for i = 0, BYTES_COUNT - 1 do
        local letter = text[i]
        counts[letter - 97] = counts[letter - 97] + 1
        code = (code * 31 + letter) % 1000000007
    end
    local pairs = 0
    for i = 1, BYTES_COUNT - 1 do
        if text[i - 1] == 119 and text[i] == 100 then pairs = pairs + 1 end
    end
    return code + counts[4] + pairs * 7
end

local function list()
    local nodes, order = {}, {}
    for i = 0, LIST_COUNT - 1 do
        order[i] = i
        nodes[i] = { value = i % 100, next = false }
    end
    for i = LIST_COUNT - 1, 1, -1 do
        local j = random() % (i + 1)
        order[i], order[j] = order[j], order[i]
    end
    for i = 0, LIST_COUNT - 2 do nodes[order[i]].next = nodes[order[i + 1]] end

    local total = 0
    for pass = 0, 19 do
        local at = nodes[order[0]]
        while at do
            total = total + at.value + pass
            at = at.next
        end
    end
    return total
end

local kernels = {
    fib = function() return fib(38) end,
    sieve = sieve,
    collatz = collatz,
    gcd = gcd_sum,
    mandelbrot = mandelbrot,
    matmul = matmul,
    particles = particle_sum,
    sort = sort,
    queens = function() return place(0) end,
    hash = hash,
    bytes = bytes,
    list = list,
}

local kernel = arg[1] or ""
if not kernels[kernel] then
    print("unknown kernel '" .. kernel .. "'")
    os.exit(2)
end
print(string.format("%.0f", kernels[kernel]()))
