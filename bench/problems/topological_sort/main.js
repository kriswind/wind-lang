// Topological sort; see main.wind.
const n = process.argv[2] === "bench" ? 1000000 : 1000;
const m = 4 * n;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const label = new Int32Array(n);
for (let i = 0; i < n; i++) label[i] = i;
for (let i = n - 1; i > 0; i--) {
    const j = random() % (i + 1);
    const held = label[i];
    label[i] = label[j];
    label[j] = held;
}
const source = new Int32Array(m);
const target = new Int32Array(m);
let edges = 0;
for (let e = 0; e < m; e++) {
    let a = random() % n;
    let b = random() % n;
    if (a === b) continue;
    if (a > b) {
        const held = a;
        a = b;
        b = held;
    }
    source[edges] = label[a];
    target[edges] = label[b];
    edges++;
}
const first = new Int32Array(n + 1);
const waiting = new Int32Array(n);
for (let e = 0; e < edges; e++) {
    first[source[e] + 1]++;
    waiting[target[e]]++;
}
for (let i = 0; i < n; i++) first[i + 1] += first[i];
const fill = first.slice(0, n);
const after = new Int32Array(edges);
for (let e = 0; e < edges; e++) after[fill[source[e]]++] = target[e];

const heap = new Int32Array(n);
let size = 0;
function push(node) {
    let at = size++;
    while (at > 0) {
        const parent = (at - 1) >> 1;
        if (heap[parent] <= node) break;
        heap[at] = heap[parent];
        at = parent;
    }
    heap[at] = node;
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

for (let i = 0; i < n; i++) {
    if (waiting[i] === 0) push(i);
}
let placed = 0;
let checksum = 0;
while (size > 0) {
    const node = pop();
    placed++;
    checksum = (checksum * 31 + node) % 1000000007;
    for (let k = first[node]; k < first[node + 1]; k++) {
        const v = after[k];
        if (--waiting[v] === 0) push(v);
    }
}
console.log(edges + " edges, " + placed + " nodes placed, checksum " + checksum);
