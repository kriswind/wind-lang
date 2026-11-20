// Longest common subsequence; see main.wind.
const n = process.argv[2] === "bench" ? 8000 : 400;
const m = Math.floor(n * 3 / 4);
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const a = new Uint8Array(n);
const b = new Uint8Array(m);
for (let i = 0; i < n; i++) a[i] = 97 + random() % 8;
for (let j = 0; j < m; j++) b[j] = 97 + random() % 8;
let previous = new Int32Array(m + 1);
let current = new Int32Array(m + 1);
for (let i = 1; i <= n; i++) {
    const letter = a[i - 1];
    for (let j = 1; j <= m; j++) {
        if (letter === b[j - 1]) current[j] = previous[j - 1] + 1;
        else current[j] = previous[j] > current[j - 1] ? previous[j] : current[j - 1];
    }
    const held = previous;
    previous = current;
    current = held;
}
console.log(String(previous[m]));
