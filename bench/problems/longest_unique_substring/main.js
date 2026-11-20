// Longest unique substring; see main.wind.
const n = process.argv[2] === "bench" ? 20000000 : 10000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const text = new Uint8Array(n);
for (let i = 0; i < n; i++) text[i] = 97 + random() % 26;
const last = new Int32Array(26).fill(-1);
let start = 0;
let best = 0;
let bestStart = 0;
for (let i = 0; i < n; i++) {
    const c = text[i] - 97;
    if (last[c] >= start) start = last[c] + 1;
    last[c] = i;
    if (i - start + 1 > best) {
        best = i - start + 1;
        bestStart = start;
    }
}
const run = String.fromCharCode(...text.subarray(bestStart, bestStart + best));
console.log(best + " letters from " + bestStart + ": " + run);
