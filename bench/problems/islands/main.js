// Counting islands; see main.wind.
const side = process.argv[2] === "bench" ? 3000 : 60;
const n = side * side;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
// 0 is water, 1 unvisited land, 2 land already counted.
const land = new Uint8Array(n);
for (let i = 0; i < n; i++) land[i] = random() % 100 < 45 ? 1 : 0;
const stack = new Int32Array(n);
let top = 0;
function visit(cell) {
    if (land[cell] === 1) {
        land[cell] = 2;
        stack[top++] = cell;
    }
}
let islands = 0;
let largest = 0;
for (let start = 0; start < n; start++) {
    if (land[start] !== 1) continue;
    islands++;
    let size = 0;
    visit(start);
    while (top > 0) {
        const cell = stack[--top];
        size++;
        const row = Math.floor(cell / side);
        const col = cell - row * side;
        if (row > 0) visit(cell - side);
        if (row < side - 1) visit(cell + side);
        if (col > 0) visit(cell - 1);
        if (col < side - 1) visit(cell + 1);
    }
    if (size > largest) largest = size;
}
console.log(islands + " islands, the largest of " + largest + " cells");
