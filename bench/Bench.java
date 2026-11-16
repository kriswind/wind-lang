// The benchmark program in Java; see bench.wind for what each kernel measures.
// It is written for speed: the particles and the list's nodes are numbers
// side by side in arrays, where objects would each be reached through a
// reference.

public class Bench {
    static final int SIEVE_LIMIT = 20000000;
    static final int SORT_COUNT = 2000000;
    static final int MATRIX_SIZE = 400;
    static final int LIST_COUNT = 1000000;
    static final int HASH_SIZE = 2097152;
    static final int HASH_KEYS = 1000000;
    static final int BYTES_COUNT = 4000000;
    static final int PARTICLE_COUNT = 1000;
    static final int QUEENS = 12;

    static final boolean[] columns = new boolean[QUEENS];
    static final boolean[] falling = new boolean[2 * QUEENS];
    static final boolean[] rising = new boolean[2 * QUEENS];

    static long seed = 42;

    static long random() {
        seed = seed * 48271 % 2147483647;
        return seed;
    }

    static long fib(long n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }

    static long sieve() {
        boolean[] composite = new boolean[SIEVE_LIMIT + 1];
        long count = 0;
        for (int i = 2; i <= SIEVE_LIMIT; i++) {
            if (composite[i]) continue;
            count++;
            for (long j = (long) i * 2; j <= SIEVE_LIMIT; j += i) composite[(int) j] = true;
        }
        return count;
    }

    static long collatz() {
        long best = 0;
        long bestStart = 0;
        for (long start = 1; start < 300000; start++) {
            long n = start;
            long steps = 0;
            while (n != 1) {
                if (n % 2 == 0) n /= 2; else n = 3 * n + 1;
                steps++;
            }
            if (steps > best) {
                best = steps;
                bestStart = start;
            }
        }
        return bestStart * 1000 + best;
    }

    static long gcd(long a, long b) {
        long x = a;
        long y = b;
        while (y != 0) {
            long rest = x % y;
            x = y;
            y = rest;
        }
        return x;
    }

    static long gcdSum() {
        long total = 0;
        for (long i = 1; i <= 1200; i++) {
            for (long j = 1; j <= 1200; j++) total += gcd(i, j);
        }
        return total;
    }

    static long mandelbrot() {
        long size = 600;
        long total = 0;
        for (long py = 0; py < size; py++) {
            for (long px = 0; px < size; px++) {
                double cx = (double) px * 3.0 / (double) size - 2.0;
                double cy = (double) py * 2.0 / (double) size - 1.0;
                double x = 0.0;
                double y = 0.0;
                long i = 0;
                while (i < 200 && x * x + y * y <= 4.0) {
                    double next = x * x - y * y + cx;
                    y = 2.0 * x * y + cy;
                    x = next;
                    i++;
                }
                total += i;
            }
        }
        return total;
    }

    static long matmul() {
        int n = MATRIX_SIZE;
        double[] left = new double[n * n];
        double[] right = new double[n * n];
        double[] product = new double[n * n];
        for (int i = 0; i < n * n; i++) {
            left[i] = (double) (i % 7) + 0.5;
            right[i] = (double) (i % 5) - 1.5;
        }
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                double sum = 0.0;
                for (int k = 0; k < n; k++) sum += left[i * n + k] * right[k * n + j];
                product[i * n + j] = sum;
            }
        }
        double total = 0.0;
        for (int i = 0; i < n * n; i++) total += product[i];
        return (long) total;
    }

    // A particle is four numbers in a row: x, y, vx, vy.
    static long particleSum() {
        double[] p = new double[PARTICLE_COUNT * 4];
        for (int i = 0; i < PARTICLE_COUNT * 4; i += 4) {
            p[i] = (double) (random() % 1000);
            p[i + 1] = (double) (random() % 1000);
            p[i + 2] = (double) (random() % 200 - 100) / 8.0;
            p[i + 3] = (double) (random() % 200 - 100) / 8.0;
        }
        for (int step = 0; step < 20000; step++) {
            for (int i = 0; i < PARTICLE_COUNT * 4; i += 4) {
                double x = p[i] + p[i + 2] * 0.5;
                double y = p[i + 1] + p[i + 3] * 0.5;
                p[i] = x;
                p[i + 1] = y;
                if (x < 0.0 || x > 1000.0) p[i + 2] = -p[i + 2];
                if (y < 0.0 || y > 1000.0) p[i + 3] = -p[i + 3];
            }
        }
        double total = 0.0;
        for (int i = 0; i < PARTICLE_COUNT * 4; i += 4) total += p[i] + p[i + 1];
        return (long) total;
    }

    static void quicksort(long[] values, int low, int high) {
        if (low >= high) return;
        long pivot = values[(low + high) / 2];
        int i = low;
        int j = high;
        while (i <= j) {
            while (values[i] < pivot) i++;
            while (values[j] > pivot) j--;
            if (i <= j) {
                long held = values[i];
                values[i] = values[j];
                values[j] = held;
                i++;
                j--;
            }
        }
        quicksort(values, low, j);
        quicksort(values, i, high);
    }

    static long sort() {
        long[] numbers = new long[SORT_COUNT];
        for (int i = 0; i < SORT_COUNT; i++) numbers[i] = random() % 1000000;
        quicksort(numbers, 0, SORT_COUNT - 1);
        long check = 0;
        for (int i = 0; i < SORT_COUNT; i += 1000) check += numbers[i] * (i % 7 + 1);
        return check;
    }

    static long place(int row) {
        if (row == QUEENS) return 1;
        long ways = 0;
        for (int column = 0; column < QUEENS; column++) {
            int down = row + column;
            int up = row - column + QUEENS;
            if (columns[column] || falling[down] || rising[up]) continue;
            columns[column] = true;
            falling[down] = true;
            rising[up] = true;
            ways += place(row + 1);
            columns[column] = false;
            falling[down] = false;
            rising[up] = false;
        }
        return ways;
    }

    static long hash() {
        long[] table = new long[HASH_SIZE];
        for (int i = 0; i < HASH_KEYS; i++) {
            long key = random();
            int slot = (int) (key % HASH_SIZE);
            while (table[slot] != 0 && table[slot] != key) slot = (slot + 1) % HASH_SIZE;
            table[slot] = key;
        }
        seed = 42;
        long found = 0;
        long probes = 0;
        for (int i = 0; i < HASH_KEYS; i++) {
            long key = random() + i % 2;
            int slot = (int) (key % HASH_SIZE);
            while (table[slot] != 0 && table[slot] != key) {
                slot = (slot + 1) % HASH_SIZE;
                probes++;
            }
            if (table[slot] == key) found++;
        }
        return found * 100 + probes;
    }

    static long bytes() {
        byte[] text = new byte[BYTES_COUNT];
        for (int i = 0; i < BYTES_COUNT; i++) text[i] = (byte) (97 + random() % 26);
        long[] counts = new long[26];
        long code = 0;
        for (int i = 0; i < BYTES_COUNT; i++) {
            long letter = text[i];
            counts[(int) letter - 97]++;
            code = (code * 31 + letter) % 1000000007;
        }
        long pairs = 0;
        for (int i = 1; i < BYTES_COUNT; i++) {
            if (text[i - 1] == 'w' && text[i] == 'd') pairs++;
        }
        return code + counts[4] + pairs * 7;
    }

    // A node is its place in two arrays: one of values, one of the place of
    // the node after it, with -1 for the end of the list.
    static long list() {
        int[] values = new int[LIST_COUNT];
        int[] nexts = new int[LIST_COUNT];
        int[] order = new int[LIST_COUNT];
        for (int i = 0; i < LIST_COUNT; i++) {
            order[i] = i;
            values[i] = i % 100;
        }
        for (int i = LIST_COUNT - 1; i > 0; i--) {
            int j = (int) (random() % (i + 1));
            int held = order[i];
            order[i] = order[j];
            order[j] = held;
        }
        for (int i = 0; i < LIST_COUNT - 1; i++) nexts[order[i]] = order[i + 1];
        nexts[order[LIST_COUNT - 1]] = -1;

        long total = 0;
        for (long pass = 0; pass < 20; pass++) {
            for (int at = order[0]; at >= 0; at = nexts[at]) total += values[at] + pass;
        }
        return total;
    }

    public static void main(String[] args) {
        String kernel = args.length > 0 ? args[0] : "";
        long result;
        switch (kernel) {
            case "fib": result = fib(38); break;
            case "sieve": result = sieve(); break;
            case "collatz": result = collatz(); break;
            case "gcd": result = gcdSum(); break;
            case "mandelbrot": result = mandelbrot(); break;
            case "matmul": result = matmul(); break;
            case "particles": result = particleSum(); break;
            case "sort": result = sort(); break;
            case "queens": result = place(0); break;
            case "hash": result = hash(); break;
            case "bytes": result = bytes(); break;
            case "list": result = list(); break;
            default:
                System.out.println("unknown kernel '" + kernel + "'");
                System.exit(2);
                return;
        }
        System.out.println(result);
    }
}
