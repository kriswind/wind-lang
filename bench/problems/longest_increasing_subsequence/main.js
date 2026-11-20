// Longest increasing subsequence; see main.wind.
const n = process.argv[2] === "bench" ? 3000000 : 5000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const values = new Int32Array(n);
for (let i = 0; i < n; i++) values[i] = random() % 1000000;
const tails = new Int32Array(n);
let length = 0;
for (let i = 0; i < n; i++) {
    const x = values[i];
    let low = 0;
    let high = length;
    while (low < high) {
        const middle = (low + high) >> 1;
        if (tails[middle] < x) low = middle + 1;
        else high = middle;
    }
    tails[low] = x;
    if (low === length) length++;
}
let sum = 0;
for (let k = 0; k < length; k++) sum += tails[k];
console.log("length " + length);
console.log("tails " + sum);
