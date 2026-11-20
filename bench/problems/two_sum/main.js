// Two sum; see main.wind.
const n = process.argv[2] === "bench" ? 2000000 : 1000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
let size = 1;
while (size < 2 * n) size *= 2;
const mask = size - 1;
const keys = new Int32Array(size).fill(-1);
const counts = new Int32Array(size);
// The slot holding key, or the empty slot (key -1) where it would go. The
// product stays below 2^53, so it is exact, and & keeps its low 32 bits.
function slot(key) {
    let at = (key * 2654435761) & mask;
    while (keys[at] !== -1 && keys[at] !== key) at = (at + 1) & mask;
    return at;
}
const values = new Int32Array(n);
for (let i = 0; i < n; i++) values[i] = random() % n;
let pairs = 0;
let distinct = 0;
for (let i = 0; i < n; i++) {
    const value = values[i];
    const want = n - value;
    let at = slot(want);
    if (keys[at] === want) pairs += counts[at];
    at = slot(value);
    if (keys[at] !== value) {
        keys[at] = value;
        distinct++;
    }
    counts[at]++;
}
console.log(pairs + " pairs, " + distinct + " distinct values");
