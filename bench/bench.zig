// The benchmark program in Zig; see bench.wind for what each kernel measures.
// It is built with ReleaseFast, which leaves out the checks on indexes and
// on arithmetic.

const std = @import("std");

const Node = struct {
    value: i64,
    next: ?*Node,
};

const Particle = struct {
    x: f64,
    y: f64,
    vx: f64,
    vy: f64,
};

const SIEVE_LIMIT = 20000000;
const SORT_COUNT = 2000000;
const MATRIX_SIZE = 400;
const LIST_COUNT = 1000000;
const HASH_SIZE = 2097152;
const HASH_KEYS = 1000000;
const BYTES_COUNT = 4000000;
const PARTICLE_COUNT = 1000;
const QUEENS = 12;

var composite: [SIEVE_LIMIT + 1]bool = @splat(false);
var numbers: [SORT_COUNT]i64 = @splat(0);
var left: [MATRIX_SIZE * MATRIX_SIZE]f64 = @splat(0.0);
var right: [MATRIX_SIZE * MATRIX_SIZE]f64 = @splat(0.0);
var product: [MATRIX_SIZE * MATRIX_SIZE]f64 = @splat(0.0);
var nodes: [LIST_COUNT]Node = @splat(.{ .value = 0, .next = null });
var order: [LIST_COUNT]usize = @splat(0);
var table: [HASH_SIZE]i64 = @splat(0);
var text: [BYTES_COUNT]u8 = @splat(0);
var counts: [26]i64 = @splat(0);
var particles: [PARTICLE_COUNT]Particle = @splat(.{ .x = 0.0, .y = 0.0, .vx = 0.0, .vy = 0.0 });
var columns: [QUEENS]bool = @splat(false);
var falling: [2 * QUEENS]bool = @splat(false);
var rising: [2 * QUEENS]bool = @splat(false);

var seed: i64 = 42;

fn random() i64 {
    seed = @rem(seed * 48271, 2147483647);
    return seed;
}

fn fib(n: i64) i64 {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

fn sieve() i64 {
    var count: i64 = 0;
    var i: usize = 2;
    while (i <= SIEVE_LIMIT) : (i += 1) {
        if (composite[i]) continue;
        count += 1;
        var j = i * 2;
        while (j <= SIEVE_LIMIT) : (j += i) composite[j] = true;
    }
    return count;
}

fn collatz() i64 {
    var best: i64 = 0;
    var best_start: i64 = 0;
    var start: i64 = 1;
    while (start < 300000) : (start += 1) {
        var n = start;
        var steps: i64 = 0;
        while (n != 1) {
            if (@rem(n, 2) == 0) n = @divTrunc(n, 2) else n = 3 * n + 1;
            steps += 1;
        }
        if (steps > best) {
            best = steps;
            best_start = start;
        }
    }
    return best_start * 1000 + best;
}

fn gcd(a: i64, b: i64) i64 {
    var x = a;
    var y = b;
    while (y != 0) {
        const rest = @rem(x, y);
        x = y;
        y = rest;
    }
    return x;
}

fn gcdSum() i64 {
    var total: i64 = 0;
    var i: i64 = 1;
    while (i <= 1200) : (i += 1) {
        var j: i64 = 1;
        while (j <= 1200) : (j += 1) total += gcd(i, j);
    }
    return total;
}

fn mandelbrot() i64 {
    const size: i64 = 600;
    var total: i64 = 0;
    var py: i64 = 0;
    while (py < size) : (py += 1) {
        var px: i64 = 0;
        while (px < size) : (px += 1) {
            const cx = @as(f64, @floatFromInt(px)) * 3.0 / @as(f64, @floatFromInt(size)) - 2.0;
            const cy = @as(f64, @floatFromInt(py)) * 2.0 / @as(f64, @floatFromInt(size)) - 1.0;
            var x: f64 = 0.0;
            var y: f64 = 0.0;
            var i: i64 = 0;
            while (i < 200 and x * x + y * y <= 4.0) {
                const next = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = next;
                i += 1;
            }
            total += i;
        }
    }
    return total;
}

fn matmul() i64 {
    const n = MATRIX_SIZE;
    for (0..n * n) |i| {
        left[i] = @as(f64, @floatFromInt(i % 7)) + 0.5;
        right[i] = @as(f64, @floatFromInt(i % 5)) - 1.5;
    }
    for (0..n) |i| {
        for (0..n) |j| {
            var sum: f64 = 0.0;
            for (0..n) |k| sum += left[i * n + k] * right[k * n + j];
            product[i * n + j] = sum;
        }
    }
    var total: f64 = 0.0;
    for (0..n * n) |i| total += product[i];
    return @intFromFloat(total);
}

fn particleSum() i64 {
    for (&particles) |*p| {
        p.x = @floatFromInt(@rem(random(), 1000));
        p.y = @floatFromInt(@rem(random(), 1000));
        p.vx = @as(f64, @floatFromInt(@rem(random(), 200) - 100)) / 8.0;
        p.vy = @as(f64, @floatFromInt(@rem(random(), 200) - 100)) / 8.0;
    }
    for (0..20000) |_| {
        for (&particles) |*p| {
            p.x += p.vx * 0.5;
            p.y += p.vy * 0.5;
            if (p.x < 0.0 or p.x > 1000.0) p.vx = -p.vx;
            if (p.y < 0.0 or p.y > 1000.0) p.vy = -p.vy;
        }
    }
    var total: f64 = 0.0;
    for (&particles) |*p| total += p.x + p.y;
    return @intFromFloat(total);
}

fn quicksort(values: [*]i64, low: isize, high: isize) void {
    if (low >= high) return;
    const pivot = values[@intCast(@divTrunc(low + high, 2))];
    var i = low;
    var j = high;
    while (i <= j) {
        while (values[@intCast(i)] < pivot) i += 1;
        while (values[@intCast(j)] > pivot) j -= 1;
        if (i <= j) {
            const held = values[@intCast(i)];
            values[@intCast(i)] = values[@intCast(j)];
            values[@intCast(j)] = held;
            i += 1;
            j -= 1;
        }
    }
    quicksort(values, low, j);
    quicksort(values, i, high);
}

fn sort() i64 {
    for (0..SORT_COUNT) |i| numbers[i] = @rem(random(), 1000000);
    quicksort(&numbers, 0, SORT_COUNT - 1);
    var check: i64 = 0;
    var i: usize = 0;
    while (i < SORT_COUNT) : (i += 1000) check += numbers[i] * @as(i64, @intCast(i % 7 + 1));
    return check;
}

fn place(row: usize) i64 {
    if (row == QUEENS) return 1;
    var ways: i64 = 0;
    for (0..QUEENS) |column| {
        const down = row + column;
        const up = row + QUEENS - column;
        if (columns[column] or falling[down] or rising[up]) continue;
        columns[column] = true;
        falling[down] = true;
        rising[up] = true;
        ways += place(row + 1);
        columns[column] = false;
        falling[down] = false;
        rising[up] = false;
    }
    return ways;
}

fn hash() i64 {
    for (0..HASH_KEYS) |_| {
        const key = random();
        var slot: usize = @intCast(@rem(key, HASH_SIZE));
        while (table[slot] != 0 and table[slot] != key) slot = (slot + 1) % HASH_SIZE;
        table[slot] = key;
    }
    seed = 42;
    var found: i64 = 0;
    var probes: i64 = 0;
    for (0..HASH_KEYS) |i| {
        const key = random() + @as(i64, @intCast(i % 2));
        var slot: usize = @intCast(@rem(key, HASH_SIZE));
        while (table[slot] != 0 and table[slot] != key) {
            slot = (slot + 1) % HASH_SIZE;
            probes += 1;
        }
        if (table[slot] == key) found += 1;
    }
    return found * 100 + probes;
}

fn bytes() i64 {
    for (0..BYTES_COUNT) |i| text[i] = @intCast(97 + @rem(random(), 26));
    var code: i64 = 0;
    for (0..BYTES_COUNT) |i| {
        const letter: i64 = text[i];
        counts[@intCast(letter - 97)] += 1;
        code = @rem(code * 31 + letter, 1000000007);
    }
    var pairs: i64 = 0;
    for (1..BYTES_COUNT) |i| {
        if (text[i - 1] == 'w' and text[i] == 'd') pairs += 1;
    }
    return code + counts[4] + pairs * 7;
}

fn list() i64 {
    for (0..LIST_COUNT) |i| {
        order[i] = i;
        nodes[i].value = @intCast(i % 100);
    }
    var i: usize = LIST_COUNT - 1;
    while (i > 0) : (i -= 1) {
        const j: usize = @intCast(@rem(random(), @as(i64, @intCast(i + 1))));
        std.mem.swap(usize, &order[i], &order[j]);
    }
    for (0..LIST_COUNT - 1) |k| nodes[order[k]].next = &nodes[order[k + 1]];
    nodes[order[LIST_COUNT - 1]].next = null;

    var total: i64 = 0;
    var pass: i64 = 0;
    while (pass < 20) : (pass += 1) {
        var at: ?*Node = &nodes[order[0]];
        while (at) |node| : (at = node.next) total += node.value + pass;
    }
    return total;
}

fn is(kernel: []const u8, name: []const u8) bool {
    return std.mem.eql(u8, kernel, name);
}

/// Writes to standard output with the system call itself, which keeps the
/// program clear of the parts of the library that differ between releases.
fn print(output: []const u8) void {
    _ = std.os.linux.write(1, output.ptr, output.len);
}

pub fn main(init: std.process.Init.Minimal) void {
    const arguments = init.args.vector;
    const kernel: []const u8 = if (arguments.len > 1) std.mem.span(arguments[1]) else "";
    var buffer: [64]u8 = undefined;
    var result: i64 = 0;
    if (is(kernel, "fib")) {
        result = fib(38);
    } else if (is(kernel, "sieve")) {
        result = sieve();
    } else if (is(kernel, "collatz")) {
        result = collatz();
    } else if (is(kernel, "gcd")) {
        result = gcdSum();
    } else if (is(kernel, "mandelbrot")) {
        result = mandelbrot();
    } else if (is(kernel, "matmul")) {
        result = matmul();
    } else if (is(kernel, "particles")) {
        result = particleSum();
    } else if (is(kernel, "sort")) {
        result = sort();
    } else if (is(kernel, "queens")) {
        result = place(0);
    } else if (is(kernel, "hash")) {
        result = hash();
    } else if (is(kernel, "bytes")) {
        result = bytes();
    } else if (is(kernel, "list")) {
        result = list();
    } else {
        print("unknown kernel\n");
        std.process.exit(2);
    }
    const line = std.fmt.bufPrint(&buffer, "{d}\n", .{result}) catch unreachable;
    print(line);
}
