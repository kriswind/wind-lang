// Dijkstra on a grid; see main.wind.
const side = process.argv[2] === "bench" ? 1500 : 40;
const n = side * side;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const cost = new Int32Array(n);
for (let i = 0; i < n; i++) cost[i] = 1 + random() % 9;
// Distances and heap keys outgrow 32 bits; doubles hold them exactly.
const dist = new Float64Array(n).fill(2 ** 62);
const heap = new Float64Array(4 * n + 1);
let size = 0;

function push(key) {
    let at = size++;
    while (at > 0) {
        const parent = (at - 1) >> 1;
        if (heap[parent] <= key) break;
        heap[at] = heap[parent];
        at = parent;
    }
    heap[at] = key;
}

function pop() {
    const top = heap[0];
    const last = heap[--size];
    let at = 0;
    for (;;) {
        let child = 2 * at + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1] < heap[child]) child++;
        if (last <= heap[child]) break;
        heap[at] = heap[child];
        at = child;
    }
    heap[at] = last;
    return top;
}

function relax(cell, d) {
    if (d < dist[cell]) {
        dist[cell] = d;
        push(d * n + cell);
    }
}

dist[0] = cost[0];
push(cost[0] * n);
while (size > 0) {
    const key = pop();
    const d = Math.floor(key / n);
    const cell = key - d * n;
    if (d > dist[cell]) continue;
    const row = Math.floor(cell / side);
    const col = cell - row * side;
    if (row > 0) relax(cell - side, d + cost[cell - side]);
    if (row < side - 1) relax(cell + side, d + cost[cell + side]);
    if (col > 0) relax(cell - 1, d + cost[cell - 1]);
    if (col < side - 1) relax(cell + 1, d + cost[cell + 1]);
}
let total = 0;
for (let i = 0; i < n; i++) total += dist[i];
console.log("the cheapest path costs " + dist[n - 1] + "; the costs to every cell sum to " + total);
