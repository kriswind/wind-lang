// Union-find; see main.wind.
const n = process.argv[2] === "bench" ? 2000000 : 1000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const parent = new Int32Array(n);
const size = new Int32Array(n);
for (let i = 0; i < n; i++) {
    parent[i] = i;
    size[i] = 1;
}
function find(node) {
    let at = node;
    while (parent[at] !== at) {
        parent[at] = parent[parent[at]];
        at = parent[at];
    }
    return at;
}
function join(a, b) {
    let x = find(a);
    let y = find(b);
    if (x === y) return false;
    if (size[x] < size[y]) {
        const held = x;
        x = y;
        y = held;
    }
    parent[y] = x;
    size[x] += size[y];
    return true;
}
let groups = n;
const joins = Math.floor(n * 9 / 10);
for (let i = 0; i < joins; i++) {
    const a = random() % n;
    const b = random() % n;
    if (join(a, b)) groups--;
}
let largest = 0;
for (let i = 0; i < n; i++) {
    if (parent[i] === i && size[i] > largest) largest = size[i];
}
console.log(groups + " groups, the largest of " + largest);
