// Merge sort inversions; see main.wind.
const n = process.argv[2] === "bench" ? 2000000 : 1000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const values = new Int32Array(n);
const scratch = new Int32Array(n);
for (let i = 0; i < n; i++) values[i] = random() % n;
function merge(low, middle, high) {
    let i = low;
    let j = middle;
    let k = low;
    let inversions = 0;
    while (i < middle && j < high) {
        if (values[j] < values[i]) {
            scratch[k++] = values[j++];
            inversions += middle - i;
        } else {
            scratch[k++] = values[i++];
        }
    }
    while (i < middle) scratch[k++] = values[i++];
    while (j < high) scratch[k++] = values[j++];
    values.set(scratch.subarray(low, high), low);
    return inversions;
}
let inversions = 0;
for (let width = 1; width < n; width *= 2) {
    for (let low = 0; low < n - width; low += 2 * width) {
        const high = low + 2 * width < n ? low + 2 * width : n;
        inversions += merge(low, low + width, high);
    }
}
let checksum = 0;
for (let i = 0; i < n; i++) checksum = (checksum * 31 + values[i]) % 1000000007;
console.log(inversions + " inversions, checksum " + checksum);
