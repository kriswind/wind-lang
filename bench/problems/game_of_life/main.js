// Game of Life; see main.wind.
const bench = process.argv[2] === "bench";
const size = bench ? 256 : 32;
const generations = bench ? 500 : 40;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
function step(source, target) {
    for (let row = 0; row < size; row++) {
        const up = (row + size - 1) % size * size;
        const middle = row * size;
        const down = (row + 1) % size * size;
        for (let column = 0; column < size; column++) {
            const left = (column + size - 1) % size;
            const right = (column + 1) % size;
            const count = source[up + left] + source[up + column] + source[up + right]
                + source[middle + left] + source[middle + right]
                + source[down + left] + source[down + column] + source[down + right];
            const alive = source[middle + column] === 1;
            target[middle + column] = count === 3 || (alive && count === 2) ? 1 : 0;
        }
    }
}
const cells = size * size;
const first = new Uint8Array(cells);
const second = new Uint8Array(cells);
for (let i = 0; i < cells; i++) first[i] = random() % 3 === 0 ? 1 : 0;
for (let g = 0; g < generations; g += 2) {
    step(first, second);
    step(second, first);
}
let alive = 0;
let checksum = 0;
for (let i = 0; i < cells; i++) {
    alive += first[i];
    checksum = (checksum * 31 + first[i]) % 1000000007;
}
console.log(alive + " alive, checksum " + checksum);
