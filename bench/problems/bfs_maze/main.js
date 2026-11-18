// Breadth-first search through a maze; see main.wind.
const side = process.argv[2] === "bench" ? 2000 : 100;
const n = side * side;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const wall = new Uint8Array(n);
for (let i = 0; i < n; i++) wall[i] = random() % 100 < 30 ? 1 : 0;
for (const cell of [0, 1, side, n - 1, n - 2, n - 1 - side]) wall[cell] = 0;
const dist = new Int32Array(n).fill(-1);
const queue = new Int32Array(n);
let head = 0;
let tail = 0;
function visit(cell, d) {
    if (wall[cell] === 0 && dist[cell] === -1) {
        dist[cell] = d;
        queue[tail++] = cell;
    }
}
dist[0] = 0;
queue[tail++] = 0;
while (head < tail) {
    const cell = queue[head++];
    const d = dist[cell] + 1;
    const row = Math.floor(cell / side);
    const col = cell - row * side;
    if (row > 0) visit(cell - side, d);
    if (row < side - 1) visit(cell + side, d);
    if (col > 0) visit(cell - 1, d);
    if (col < side - 1) visit(cell + 1, d);
}
console.log(dist[n - 1] + " steps, " + tail + " cells reached");
