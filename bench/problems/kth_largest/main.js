// Kth largest; see main.wind.
const bench = process.argv[2] === "bench";
const n = bench ? 30000000 : 10000;
const k = bench ? 100000 : 100;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const heap = new Int32Array(k);
let size = 0;
function push(value) {
    let at = size++;
    heap[at] = value;
    while (at > 0) {
        const parent = (at - 1) >> 1;
        if (heap[parent] <= heap[at]) break;
        const held = heap[parent];
        heap[parent] = heap[at];
        heap[at] = held;
        at = parent;
    }
}
function replaceRoot(value) {
    heap[0] = value;
    let at = 0;
    for (;;) {
        let child = 2 * at + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1] < heap[child]) child++;
        if (heap[at] <= heap[child]) break;
        const held = heap[child];
        heap[child] = heap[at];
        heap[at] = held;
        at = child;
    }
}
for (let i = 0; i < n; i++) {
    const value = random();
    if (size < k) push(value);
    else if (value > heap[0]) replaceRoot(value);
}
let sum = 0;
for (let i = 0; i < size; i++) sum += heap[i];
console.log("sum " + sum + ", k-th largest " + heap[0]);
