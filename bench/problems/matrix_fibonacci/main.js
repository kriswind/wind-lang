// Matrix Fibonacci; see main.wind.
const PRIME = 67108859;
const queries = process.argv[2] === "bench" ? 1000000 : 1000;
let seed = 42;
function random() {
    seed = seed * 48271 % 2147483647;
    return seed;
}
function multiply(x, y) {
    return {
        a: (x.a * y.a + x.b * y.c) % PRIME, b: (x.a * y.b + x.b * y.d) % PRIME,
        c: (x.c * y.a + x.d * y.c) % PRIME, d: (x.c * y.b + x.d * y.d) % PRIME,
    };
}
function fibonacci(n) {
    let result = { a: 1, b: 0, c: 0, d: 1 };
    let base = { a: 1, b: 1, c: 1, d: 0 };
    let left = n;
    while (left > 0) {
        if ((left & 1) === 1) result = multiply(result, base);
        base = multiply(base, base);
        left >>= 1;
    }
    return result.b;
}
let total = 0;
for (let i = 0; i < queries; i++) total = (total + fibonacci(random())) % PRIME;
console.log(String(total));
