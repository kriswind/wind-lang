// The benchmark program in Swift; see bench.wind for what each kernel
// measures. It is written for speed: it is built without overflow or bounds
// checks, the kernels that index heavily work on plain memory, and the list's
// nodes are structs reached through pointers, so that nothing is counted.

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

struct Node {
    var value = 0
    var next: UnsafeMutablePointer<Node>? = nil
}

struct Particle {
    var x = 0.0
    var y = 0.0
    var vx = 0.0
    var vy = 0.0
}

let sieveLimit = 20000000
let sortCount = 2000000
let matrixSize = 400
let listCount = 1000000
let hashSize = 2097152
let hashKeys = 1000000
let bytesCount = 4000000
let particleCount = 1000
let queens = 12

struct Random {
    var seed = 42

    mutating func next() -> Int {
        seed = seed * 48271 % 2147483647
        return seed
    }
}

func fib(_ n: Int) -> Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

func sieve() -> Int {
    let composite = UnsafeMutablePointer<Bool>.allocate(capacity: sieveLimit + 1)
    composite.initialize(repeating: false, count: sieveLimit + 1)
    defer { composite.deallocate() }
    var count = 0
    var i = 2
    while i <= sieveLimit {
        if !composite[i] {
            count += 1
            var j = i * 2
            while j <= sieveLimit {
                composite[j] = true
                j += i
            }
        }
        i += 1
    }
    return count
}

func collatz() -> Int {
    var best = 0
    var bestStart = 0
    for start in 1..<300000 {
        var n = start
        var steps = 0
        while n != 1 {
            if n % 2 == 0 { n /= 2 } else { n = 3 * n + 1 }
            steps += 1
        }
        if steps > best {
            best = steps
            bestStart = start
        }
    }
    return bestStart * 1000 + best
}

func gcd(_ a: Int, _ b: Int) -> Int {
    var x = a
    var y = b
    while y != 0 {
        let rest = x % y
        x = y
        y = rest
    }
    return x
}

func gcdSum() -> Int {
    var total = 0
    for i in 1...1200 {
        for j in 1...1200 { total += gcd(i, j) }
    }
    return total
}

func mandelbrot() -> Int {
    let size = 600
    var total = 0
    for py in 0..<size {
        for px in 0..<size {
            let cx = Double(px) * 3.0 / Double(size) - 2.0
            let cy = Double(py) * 2.0 / Double(size) - 1.0
            var x = 0.0
            var y = 0.0
            var i = 0
            while i < 200 && x * x + y * y <= 4.0 {
                let next = x * x - y * y + cx
                y = 2.0 * x * y + cy
                x = next
                i += 1
            }
            total += i
        }
    }
    return total
}

func matmul() -> Int {
    let n = matrixSize
    var left = [Double](repeating: 0.0, count: n * n)
    var right = [Double](repeating: 0.0, count: n * n)
    var product = [Double](repeating: 0.0, count: n * n)
    for i in 0..<n * n {
        left[i] = Double(i % 7) + 0.5
        right[i] = Double(i % 5) - 1.5
    }
    for i in 0..<n {
        for j in 0..<n {
            var sum = 0.0
            for k in 0..<n { sum += left[i * n + k] * right[k * n + j] }
            product[i * n + j] = sum
        }
    }
    var total = 0.0
    for i in 0..<n * n { total += product[i] }
    return Int(total)
}

func particleSum(_ random: inout Random) -> Int {
    var particles = [Particle](repeating: Particle(), count: particleCount)
    for i in 0..<particleCount {
        particles[i].x = Double(random.next() % 1000)
        particles[i].y = Double(random.next() % 1000)
        particles[i].vx = Double(random.next() % 200 - 100) / 8.0
        particles[i].vy = Double(random.next() % 200 - 100) / 8.0
    }
    for _ in 0..<20000 {
        for i in 0..<particleCount {
            particles[i].x += particles[i].vx * 0.5
            particles[i].y += particles[i].vy * 0.5
            if particles[i].x < 0.0 || particles[i].x > 1000.0 { particles[i].vx = -particles[i].vx }
            if particles[i].y < 0.0 || particles[i].y > 1000.0 { particles[i].vy = -particles[i].vy }
        }
    }
    var total = 0.0
    for i in 0..<particleCount { total += particles[i].x + particles[i].y }
    return Int(total)
}

func quicksort(_ values: UnsafeMutablePointer<Int>, _ low: Int, _ high: Int) {
    if low >= high { return }
    let pivot = values[(low + high) / 2]
    var i = low
    var j = high
    while i <= j {
        while values[i] < pivot { i += 1 }
        while values[j] > pivot { j -= 1 }
        if i <= j {
            let held = values[i]
            values[i] = values[j]
            values[j] = held
            i += 1
            j -= 1
        }
    }
    quicksort(values, low, j)
    quicksort(values, i, high)
}

func sort(_ random: inout Random) -> Int {
    let numbers = UnsafeMutablePointer<Int>.allocate(capacity: sortCount)
    defer { numbers.deallocate() }
    for i in 0..<sortCount { numbers[i] = random.next() % 1000000 }
    quicksort(numbers, 0, sortCount - 1)
    var check = 0
    for i in stride(from: 0, to: sortCount, by: 1000) { check += numbers[i] * (i % 7 + 1) }
    return check
}

struct Board {
    let columns: UnsafeMutablePointer<Bool>
    let falling: UnsafeMutablePointer<Bool>
    let rising: UnsafeMutablePointer<Bool>

    init() {
        columns = .allocate(capacity: queens)
        falling = .allocate(capacity: 2 * queens)
        rising = .allocate(capacity: 2 * queens)
        columns.initialize(repeating: false, count: queens)
        falling.initialize(repeating: false, count: 2 * queens)
        rising.initialize(repeating: false, count: 2 * queens)
    }
}

func place(_ board: Board, _ row: Int) -> Int {
    if row == queens { return 1 }
    var ways = 0
    for column in 0..<queens {
        let down = row + column
        let up = row - column + queens
        if board.columns[column] || board.falling[down] || board.rising[up] { continue }
        board.columns[column] = true
        board.falling[down] = true
        board.rising[up] = true
        ways += place(board, row + 1)
        board.columns[column] = false
        board.falling[down] = false
        board.rising[up] = false
    }
    return ways
}

func hash(_ random: inout Random) -> Int {
    let table = UnsafeMutablePointer<Int>.allocate(capacity: hashSize)
    table.initialize(repeating: 0, count: hashSize)
    defer { table.deallocate() }
    for _ in 0..<hashKeys {
        let key = random.next()
        var slot = key % hashSize
        while table[slot] != 0 && table[slot] != key { slot = (slot + 1) % hashSize }
        table[slot] = key
    }
    random.seed = 42
    var found = 0
    var probes = 0
    for i in 0..<hashKeys {
        let key = random.next() + i % 2
        var slot = key % hashSize
        while table[slot] != 0 && table[slot] != key {
            slot = (slot + 1) % hashSize
            probes += 1
        }
        if table[slot] == key { found += 1 }
    }
    return found * 100 + probes
}

func bytes(_ random: inout Random) -> Int {
    var text = [UInt8](repeating: 0, count: bytesCount)
    for i in 0..<bytesCount { text[i] = UInt8(97 + random.next() % 26) }
    var counts = [Int](repeating: 0, count: 26)
    var code = 0
    for i in 0..<bytesCount {
        let letter = Int(text[i])
        counts[letter - 97] += 1
        code = (code * 31 + letter) % 1000000007
    }
    var pairs = 0
    for i in 1..<bytesCount {
        if text[i - 1] == 119 && text[i] == 100 { pairs += 1 }
    }
    return code + counts[4] + pairs * 7
}

func list(_ random: inout Random) -> Int {
    let nodes = UnsafeMutablePointer<Node>.allocate(capacity: listCount)
    nodes.initialize(repeating: Node(), count: listCount)
    defer { nodes.deallocate() }
    var order = [Int](repeating: 0, count: listCount)
    for i in 0..<listCount {
        order[i] = i
        nodes[i].value = i % 100
    }
    for i in stride(from: listCount - 1, to: 0, by: -1) {
        let j = random.next() % (i + 1)
        order.swapAt(i, j)
    }
    for i in 0..<listCount - 1 { nodes[order[i]].next = nodes + order[i + 1] }

    var total = 0
    for pass in 0..<20 {
        var at: UnsafeMutablePointer<Node>? = nodes + order[0]
        while let node = at {
            total += node.pointee.value + pass
            at = node.pointee.next
        }
    }
    return total
}

let kernel = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ""
var random = Random()
let board = Board()
let result: Int
switch kernel {
case "fib": result = fib(38)
case "sieve": result = sieve()
case "collatz": result = collatz()
case "gcd": result = gcdSum()
case "mandelbrot": result = mandelbrot()
case "matmul": result = matmul()
case "particles": result = particleSum(&random)
case "sort": result = sort(&random)
case "queens": result = place(board, 0)
case "hash": result = hash(&random)
case "bytes": result = bytes(&random)
case "list": result = list(&random)
default:
    print("unknown kernel '\(kernel)'")
    exit(2)
}
print(result)
