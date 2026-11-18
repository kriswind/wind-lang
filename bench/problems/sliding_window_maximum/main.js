// Sliding window maximum; see main.wind.
const bench = process.argv[2] === "bench";
const n = bench ? 10000000 : 1000;
const k = bench ? 1000 : 16;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const values = new Int32Array(n);
for (let i = 0; i < n; i++) values[i] = random() % 1000000;
const deque = new Int32Array(n);
let head = 0;
let tail = 0;
let sum = 0;
for (let i = 0; i < n; i++) {
    while (tail > head && values[deque[tail - 1]] <= values[i]) tail--;
    deque[tail++] = i;
    if (deque[head] <= i - k) head++;
    if (i >= k - 1) sum += values[deque[head]];
}
console.log((n - k + 1) + " windows of " + k + ", the maxima summing to " + sum);
