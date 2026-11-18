// Next permutation; see main.wind.
const bench = process.argv[2] === "bench";
const n = bench ? 10 : 7;
const rounds = bench ? 4 : 2;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const items = new Int32Array(n);
const weights = new Int32Array(n);
function nextPermutation() {
    let i = n - 2;
    while (i >= 0 && items[i] >= items[i + 1]) i--;
    if (i < 0) return false;
    let j = n - 1;
    while (items[j] <= items[i]) j--;
    let held = items[i];
    items[i] = items[j];
    items[j] = held;
    for (let low = i + 1, high = n - 1; low < high; low++, high--) {
        held = items[low];
        items[low] = items[high];
        items[high] = held;
    }
    return true;
}
let count = 0;
let total = 0;
let checksum = 0;
for (let round = 0; round < rounds; round++) {
    for (let i = 0; i < n; i++) {
        items[i] = i;
        weights[i] = random() % 1000 + 1;
    }
    do {
        let value = 0;
        for (let i = 0; i < n; i++) value += items[i] * weights[i];
        count++;
        total += value;
        checksum = (checksum * 31 + value) % 1000000007;
    } while (nextPermutation());
}
console.log(count + " permutations, total " + total + ", checksum " + checksum);
