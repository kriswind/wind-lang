// Coin change; see main.wind.
const bench = process.argv[2] === "bench";
const count = bench ? 40 : 8;
const limit = bench ? 1000000 : 2000;
const PRIME = 1000000007;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
const coins = [];
for (let i = 0; i < count; i++) {
    let coin = 2 + random() % 199;
    while (coins.includes(coin)) coin = 2 + random() % 199;
    coins.push(coin);
}

const ways = new Int32Array(limit + 1);
ways[0] = 1;
for (const coin of coins) {
    for (let a = coin; a <= limit; a++) {
        let total = ways[a] + ways[a - coin];
        if (total >= PRIME) total -= PRIME;
        ways[a] = total;
    }
}

const fewest = new Int32Array(limit + 1).fill(limit + 1);
fewest[0] = 0;
for (const coin of coins) {
    for (let a = coin; a <= limit; a++) {
        const used = fewest[a - coin] + 1;
        if (used < fewest[a]) fewest[a] = used;
    }
}

let waysSum = 0;
let fewestSum = 0;
let unreachable = 0;
for (let a = 1; a <= limit; a++) {
    waysSum += ways[a];
    if (waysSum >= PRIME) waysSum -= PRIME;
    if (fewest[a] > limit) unreachable++;
    else fewestSum += fewest[a];
}
console.log("ways " + waysSum);
console.log("fewest " + fewestSum);
console.log("unreachable " + unreachable);
