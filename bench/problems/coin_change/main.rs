// Coin change; see main.wind.
use std::env;

const PRIME: i64 = 1000000007;

fn main() {
    let bench = env::args().nth(1).map_or(false, |s| s == "bench");
    let count = if bench { 40 } else { 8 };
    let limit: usize = if bench { 1000000 } else { 2000 };
    let mut seed: i64 = 42;
    let mut random = || {
        seed = seed * 48271 % 2147483647;
        seed
    };
    let mut coins: Vec<usize> = Vec::with_capacity(count);
    for _ in 0..count {
        let mut coin = (2 + random() % 199) as usize;
        while coins.contains(&coin) {
            coin = (2 + random() % 199) as usize;
        }
        coins.push(coin);
    }

    let mut ways = vec![0i64; limit + 1];
    ways[0] = 1;
    for &coin in &coins {
        for a in coin..=limit {
            let mut total = ways[a] + ways[a - coin];
            if total >= PRIME {
                total -= PRIME;
            }
            ways[a] = total;
        }
    }

    let unmade = limit as i64 + 1;
    let mut fewest = vec![unmade; limit + 1];
    fewest[0] = 0;
    for &coin in &coins {
        for a in coin..=limit {
            let used = fewest[a - coin] + 1;
            if used < fewest[a] {
                fewest[a] = used;
            }
        }
    }

    let (mut ways_sum, mut fewest_sum, mut unreachable) = (0i64, 0i64, 0i64);
    for a in 1..=limit {
        ways_sum += ways[a];
        if ways_sum >= PRIME {
            ways_sum -= PRIME;
        }
        if fewest[a] == unmade {
            unreachable += 1;
        } else {
            fewest_sum += fewest[a];
        }
    }
    println!("ways {}", ways_sum);
    println!("fewest {}", fewest_sum);
    println!("unreachable {}", unreachable);
}
