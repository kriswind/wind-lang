// Maximum subarray; see main.wind.
const n = process.argv[2] === "bench" ? 10000000 : 1000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const values = new Int32Array(n);
for (let i = 0; i < n; i++) values[i] = random() % 2001 - 1000;
let best = values[0];
let bestStart = 0;
let bestEnd = 0;
let sum = values[0];
let start = 0;
for (let i = 1; i < n; i++) {
    if (sum <= 0) {
        sum = values[i];
        start = i;
    } else {
        sum += values[i];
    }
    if (sum > best) {
        best = sum;
        bestStart = start;
        bestEnd = i;
    }
}
console.log(best + " from " + bestStart + " to " + bestEnd);
