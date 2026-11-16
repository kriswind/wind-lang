// The benchmark program in JavaScript; see bench.wind for what each kernel
// measures. It is written for speed: every array is a typed array, the
// particles and the list's nodes are numbers side by side in such arrays
// where objects would be, and whole numbers are kept within 32 bits where
// the engine can then work on them as integers.

const SIEVE_LIMIT = 20000000;
const SORT_COUNT = 2000000;
const MATRIX_SIZE = 400;
const LIST_COUNT = 1000000;
const HASH_SIZE = 2097152;
const HASH_KEYS = 1000000;
const BYTES_COUNT = 4000000;
const PARTICLE_COUNT = 1000;
const QUEENS = 12;

const columns = new Uint8Array(QUEENS);
const falling = new Uint8Array(2 * QUEENS);
const rising = new Uint8Array(2 * QUEENS);

let seed = 42;

// The same numbers as "seed * 48271 % 2147483647", worked out in steps
// that each fit in 32 bits (Schrage's method), which is far quicker here
// than a remainder of a 47-bit product.
function random() {
    const next = 48271 * (seed % 44488) - 3399 * ((seed / 44488) | 0);
    seed = next < 0 ? next + 2147483647 : next;
    return seed;
}

function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

function sieve() {
    const composite = new Uint8Array(SIEVE_LIMIT + 1);
    let count = 0;
    for (let i = 2; i <= SIEVE_LIMIT; i++) {
        if (composite[i]) continue;
        count++;
        for (let j = i * 2; j <= SIEVE_LIMIT; j += i) composite[j] = 1;
    }
    return count;
}

function collatz() {
    let best = 0;
    let bestStart = 0;
    for (let start = 1; start < 300000; start++) {
        let n = start;
        let steps = 0;
        // n outgrows 32 bits, but its lowest bit says whether it is even.
        while (n !== 1) {
            if ((n & 1) === 0) n /= 2; else n = 3 * n + 1;
            steps++;
        }
        if (steps > best) {
            best = steps;
            bestStart = start;
        }
    }
    return bestStart * 1000 + best;
}

function gcd(a, b) {
    let x = a;
    let y = b;
    while (y !== 0) {
        const rest = x % y;
        x = y;
        y = rest;
    }
    return x;
}

function gcdSum() {
    let total = 0;
    for (let i = 1; i <= 1200; i++) {
        for (let j = 1; j <= 1200; j++) total += gcd(i, j);
    }
    return total;
}

function mandelbrot() {
    const size = 600;
    let total = 0;
    for (let py = 0; py < size; py++) {
        for (let px = 0; px < size; px++) {
            const cx = px * 3.0 / size - 2.0;
            const cy = py * 2.0 / size - 1.0;
            let x = 0.0;
            let y = 0.0;
            let i = 0;
            while (i < 200 && x * x + y * y <= 4.0) {
                const next = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = next;
                i++;
            }
            total += i;
        }
    }
    return total;
}

function matmul() {
    const n = MATRIX_SIZE;
    const left = new Float64Array(n * n);
    const right = new Float64Array(n * n);
    const product = new Float64Array(n * n);
    for (let i = 0; i < n * n; i++) {
        left[i] = i % 7 + 0.5;
        right[i] = i % 5 - 1.5;
    }
    for (let i = 0; i < n; i++) {
        for (let j = 0; j < n; j++) {
            let sum = 0.0;
            for (let k = 0; k < n; k++) sum += left[i * n + k] * right[k * n + j];
            product[i * n + j] = sum;
        }
    }
    let total = 0.0;
    for (let i = 0; i < n * n; i++) total += product[i];
    return Math.trunc(total);
}

// A particle is four numbers in a row: x, y, vx, vy.
function particleSum() {
    const p = new Float64Array(PARTICLE_COUNT * 4);
    for (let i = 0; i < PARTICLE_COUNT * 4; i += 4) {
        p[i] = random() % 1000;
        p[i + 1] = random() % 1000;
        p[i + 2] = (random() % 200 - 100) / 8.0;
        p[i + 3] = (random() % 200 - 100) / 8.0;
    }
    for (let step = 0; step < 20000; step++) {
        for (let i = 0; i < PARTICLE_COUNT * 4; i += 4) {
            const x = p[i] + p[i + 2] * 0.5;
            const y = p[i + 1] + p[i + 3] * 0.5;
            p[i] = x;
            p[i + 1] = y;
            if (x < 0.0 || x > 1000.0) p[i + 2] = -p[i + 2];
            if (y < 0.0 || y > 1000.0) p[i + 3] = -p[i + 3];
        }
    }
    let total = 0.0;
    for (let i = 0; i < PARTICLE_COUNT * 4; i += 4) total += p[i] + p[i + 1];
    return Math.trunc(total);
}

function quicksort(values, low, high) {
    if (low >= high) return;
    const pivot = values[(low + high) >> 1];
    let i = low;
    let j = high;
    while (i <= j) {
        while (values[i] < pivot) i++;
        while (values[j] > pivot) j--;
        if (i <= j) {
            const held = values[i];
            values[i] = values[j];
            values[j] = held;
            i++;
            j--;
        }
    }
    quicksort(values, low, j);
    quicksort(values, i, high);
}

function sort() {
    const numbers = new Int32Array(SORT_COUNT);
    for (let i = 0; i < SORT_COUNT; i++) numbers[i] = random() % 1000000;
    quicksort(numbers, 0, SORT_COUNT - 1);
    let check = 0;
    for (let i = 0; i < SORT_COUNT; i += 1000) check += numbers[i] * (i % 7 + 1);
    return check;
}

function place(row) {
    if (row === QUEENS) return 1;
    let ways = 0;
    for (let column = 0; column < QUEENS; column++) {
        const down = row + column;
        const up = row - column + QUEENS;
        if (columns[column] || falling[down] || rising[up]) continue;
        columns[column] = 1;
        falling[down] = 1;
        rising[up] = 1;
        ways += place(row + 1);
        columns[column] = 0;
        falling[down] = 0;
        rising[up] = 0;
    }
    return ways;
}

function hash() {
    // The table's size is a power of two, so a mask finds the slot.
    const mask = HASH_SIZE - 1;
    const table = new Int32Array(HASH_SIZE);
    for (let i = 0; i < HASH_KEYS; i++) {
        const key = random() | 0;
        let slot = key & mask;
        while (table[slot] !== 0 && table[slot] !== key) slot = (slot + 1) & mask;
        table[slot] = key;
    }
    seed = 42;
    let found = 0;
    let probes = 0;
    for (let i = 0; i < HASH_KEYS; i++) {
        const key = (random() + (i & 1)) | 0;
        let slot = key & mask;
        while (table[slot] !== 0 && table[slot] !== key) {
            slot = (slot + 1) & mask;
            probes++;
        }
        if (table[slot] === key) found++;
    }
    return found * 100 + probes;
}

function bytes() {
    const text = new Uint8Array(BYTES_COUNT);
    for (let i = 0; i < BYTES_COUNT; i++) text[i] = 97 + random() % 26;
    const counts = new Int32Array(26);
    let code = 0;
    for (let i = 0; i < BYTES_COUNT; i++) {
        const letter = text[i];
        counts[letter - 97]++;
        // (code * 31 + letter) % 1000000007, again within 32 bits.
        let next = 31 * (code % 32258064) - 23 * ((code / 32258064) | 0);
        if (next < 0) next += 1000000007;
        next += letter;
        code = next >= 1000000007 ? next - 1000000007 : next;
    }
    let pairs = 0;
    for (let i = 1; i < BYTES_COUNT; i++) {
        if (text[i - 1] === 119 && text[i] === 100) pairs++;
    }
    return code + counts[4] + pairs * 7;
}

// A node is its place in two arrays: one of values, one of the place of the
// node after it, with -1 for the end of the list.
function list() {
    const values = new Int32Array(LIST_COUNT);
    const nexts = new Int32Array(LIST_COUNT);
    const order = new Int32Array(LIST_COUNT);
    for (let i = 0; i < LIST_COUNT; i++) {
        order[i] = i;
        values[i] = i % 100;
    }
    for (let i = LIST_COUNT - 1; i > 0; i--) {
        const j = random() % (i + 1);
        const held = order[i];
        order[i] = order[j];
        order[j] = held;
    }
    for (let i = 0; i < LIST_COUNT - 1; i++) nexts[order[i]] = order[i + 1];
    nexts[order[LIST_COUNT - 1]] = -1;

    let total = 0;
    for (let pass = 0; pass < 20; pass++) {
        for (let at = order[0]; at >= 0; at = nexts[at]) total += values[at] + pass;
    }
    return total;
}

const kernels = {
    fib: () => fib(38),
    sieve,
    collatz,
    gcd: gcdSum,
    mandelbrot,
    matmul,
    particles: particleSum,
    sort,
    queens: () => place(0),
    hash,
    bytes,
    list,
};

const kernel = process.argv[2] || "";
if (!Object.prototype.hasOwnProperty.call(kernels, kernel)) {
    console.log("unknown kernel '" + kernel + "'");
    process.exit(2);
}
console.log(String(kernels[kernel]()));
