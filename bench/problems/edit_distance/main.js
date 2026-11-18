// Edit distance; see main.wind.
const n = process.argv[2] === "bench" ? 6000 : 300;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const a = new Uint8Array(n);
const b = new Uint8Array(n);
for (let i = 0; i < n; i++) a[i] = 97 + random() % 4;
for (let i = 0; i < n; i++) b[i] = 97 + random() % 4;
let previous = new Int32Array(n + 1);
let current = new Int32Array(n + 1);
for (let j = 0; j <= n; j++) previous[j] = j;
for (let i = 1; i <= n; i++) {
    current[0] = i;
    for (let j = 1; j <= n; j++) {
        const cost = a[i - 1] === b[j - 1] ? 0 : 1;
        let best = previous[j - 1] + cost;
        if (previous[j] + 1 < best) best = previous[j] + 1;
        if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
        current[j] = best;
    }
    const held = previous;
    previous = current;
    current = held;
}
console.log(String(previous[n]));
