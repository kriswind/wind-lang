// 0/1 knapsack; see main.wind.
const n = process.argv[2] === "bench" ? 1000 : 60;
const capacity = n * 125;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const weight = new Int32Array(n);
const value = new Int32Array(n);
for (let i = 0; i < n; i++) {
    weight[i] = 1 + random() % 1000;
    value[i] = 1 + random() % 1000;
}
const best = new Int32Array(capacity + 1);
for (let i = 0; i < n; i++) {
    const w = weight[i];
    const v = value[i];
    for (let c = capacity; c >= w; c--) {
        const taken = best[c - w] + v;
        if (taken > best[c]) best[c] = taken;
    }
}
console.log(String(best[capacity]));
