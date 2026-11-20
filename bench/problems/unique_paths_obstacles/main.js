// Unique paths with obstacles; see main.wind.
const size = process.argv[2] === "bench" ? 5000 : 120;
const rows = size;
const columns = size;
const PRIME = 1000000007;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const paths = new Int32Array(columns);
let obstacles = 0;
for (let i = 0; i < rows; i++) {
    for (let j = 0; j < columns; j++) {
        const corner = (i === 0 && j === 0) || (i === rows - 1 && j === columns - 1);
        if (random() % 10 === 0 && !corner) {
            obstacles++;
            paths[j] = 0;
        } else if (i === 0 && j === 0) {
            paths[j] = 1;
        } else if (j > 0) {
            let total = paths[j] + paths[j - 1];
            if (total >= PRIME) total -= PRIME;
            paths[j] = total;
        }
    }
}
console.log("paths " + paths[columns - 1]);
console.log("obstacles " + obstacles);
