{-# LANGUAGE BangPatterns #-}

-- The benchmark program in Haskell; see bench.wind for what each kernel
-- measures. It is written for speed in the way Haskell allows: arrays are
-- unboxed and changed in place, their indexes are not checked, loops are
-- strict recursion, halving is a shift, and the linked list is two arrays
-- of numbers.
module Main (main) where

import Control.Monad (when)
import Data.Array.Base (unsafeRead, unsafeWrite)
import Data.Array.IO (IOUArray, newArray)
import Data.Bits (unsafeShiftR, (.&.))
import Data.Word (Word8)
import System.Environment (getArgs)
import System.Exit (ExitCode (..), exitWith)

sieveLimit, sortCount, matrixSize, listCount, hashSize, hashKeys, bytesCount, particleCount, queens :: Int
sieveLimit = 20000000
sortCount = 2000000
matrixSize = 400
listCount = 1000000
hashSize = 2097152
hashKeys = 1000000
bytesCount = 4000000
particleCount = 1000
queens = 12

type Ints = IOUArray Int Int
type Floats = IOUArray Int Double
type Bytes = IOUArray Int Word8

-- The generator's state is the one number in an array of its own.
random :: Ints -> IO Int
random seed = do
  s <- unsafeRead seed 0
  let !next = s * 48271 `rem` 2147483647
  unsafeWrite seed 0 next
  return next

-- Runs an action for each number from a start up to, not including, an end.
loop :: Int -> Int -> (Int -> IO ()) -> IO ()
loop !from !to body = go from
  where
    go !i = when (i < to) (body i >> go (i + 1))
{-# INLINE loop #-}

fib :: Int -> Int
fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

sieve :: IO Int
sieve = do
  composite <- newArray (0, sieveLimit) 0 :: IO Bytes
  let mark !j !i = when (j <= sieveLimit) (unsafeWrite composite j 1 >> mark (j + i) i)
      go !i !count
        | i > sieveLimit = return count
        | otherwise = do
            c <- unsafeRead composite i
            if c /= 0 then go (i + 1) count else mark (i * 2) i >> go (i + 1) (count + 1)
  go 2 0

collatz :: Int
collatz = go 1 0 0
  where
    go :: Int -> Int -> Int -> Int
    go !start !best !bestStart
      | start >= 300000 = bestStart * 1000 + best
      | otherwise =
          let !steps = chain start 0
           in if steps > best then go (start + 1) steps start else go (start + 1) best bestStart
    chain :: Int -> Int -> Int
    chain !n !steps
      | n == 1 = steps
      | n .&. 1 == 0 = chain (n `unsafeShiftR` 1) (steps + 1)
      | otherwise = chain (3 * n + 1) (steps + 1)

gcdOf :: Int -> Int -> Int
gcdOf !x !y = if y == 0 then x else gcdOf y (x `rem` y)

gcdSum :: Int
gcdSum = outer 1 0
  where
    outer :: Int -> Int -> Int
    outer !i !total = if i > 1200 then total else outer (i + 1) (inner i 1 total)
    inner :: Int -> Int -> Int -> Int
    inner !i !j !total = if j > 1200 then total else inner i (j + 1) (total + gcdOf i j)

mandelbrot :: Int
mandelbrot = rows 0 0
  where
    size = 600 :: Int
    rows :: Int -> Int -> Int
    rows !py !total = if py >= size then total else rows (py + 1) (cells py 0 total)
    cells :: Int -> Int -> Int -> Int
    cells !py !px !total
      | px >= size = total
      | otherwise =
          let !cx = fromIntegral px * 3.0 / fromIntegral size - 2.0
              !cy = fromIntegral py * 2.0 / fromIntegral size - 1.0
           in cells py (px + 1) (total + escape cx cy 0.0 0.0 0)
    escape :: Double -> Double -> Double -> Double -> Int -> Int
    escape !cx !cy !x !y !i
      | i < 200 && x * x + y * y <= 4.0 = escape cx cy (x * x - y * y + cx) (2.0 * x * y + cy) (i + 1)
      | otherwise = i

matmul :: IO Int
matmul = do
  let n = matrixSize
  left <- newArray (0, n * n - 1) 0.0 :: IO Floats
  right <- newArray (0, n * n - 1) 0.0 :: IO Floats
  result <- newArray (0, n * n - 1) 0.0 :: IO Floats
  loop 0 (n * n) $ \i -> do
    unsafeWrite left i (fromIntegral (i `rem` 7) + 0.5)
    unsafeWrite right i (fromIntegral (i `rem` 5) - 1.5)
  let dot !i !j !k !acc
        | k >= n = return acc
        | otherwise = do
            a <- unsafeRead left (i * n + k)
            b <- unsafeRead right (k * n + j)
            dot i j (k + 1) (acc + a * b)
  loop 0 n $ \i ->
    loop 0 n $ \j -> do
      s <- dot i j 0 0.0
      unsafeWrite result (i * n + j) s
  let total !i !acc
        | i >= n * n = return acc
        | otherwise = unsafeRead result i >>= \v -> total (i + 1) (acc + v)
  t <- total 0 (0.0 :: Double)
  return (truncate t)

-- A particle is four numbers side by side in one array: x, y, vx, vy.
particleSum :: Ints -> IO Int
particleSum seed = do
  ps <- newArray (0, particleCount * 4 - 1) 0.0 :: IO Floats
  loop 0 particleCount $ \i -> do
    x <- random seed
    y <- random seed
    vx <- random seed
    vy <- random seed
    unsafeWrite ps (i * 4) (fromIntegral (x `rem` 1000))
    unsafeWrite ps (i * 4 + 1) (fromIntegral (y `rem` 1000))
    unsafeWrite ps (i * 4 + 2) (fromIntegral (vx `rem` 200 - 100) / 8.0)
    unsafeWrite ps (i * 4 + 3) (fromIntegral (vy `rem` 200 - 100) / 8.0)
  let move !at = do
        p <- unsafeRead ps at
        v <- unsafeRead ps (at + 2)
        let !q = p + v * 0.5
        unsafeWrite ps at q
        when (q < 0.0 || q > 1000.0) (unsafeWrite ps (at + 2) (negate v))
  loop 0 20000 $ \_ ->
    loop 0 particleCount $ \i -> move (i * 4) >> move (i * 4 + 1)
  let total !i !acc
        | i >= particleCount = return acc
        | otherwise = do
            x <- unsafeRead ps (i * 4)
            y <- unsafeRead ps (i * 4 + 1)
            total (i + 1) (acc + (x + y))
  t <- total 0 (0.0 :: Double)
  return (truncate t)

quicksort :: Ints -> Int -> Int -> IO ()
quicksort values !low !high = when (low < high) $ do
  pivot <- unsafeRead values ((low + high) `quot` 2)
  let up !i = unsafeRead values i >>= \v -> if v < pivot then up (i + 1) else return i
      down !j = unsafeRead values j >>= \v -> if v > pivot then down (j - 1) else return j
      go !i !j
        | i > j = return (i, j)
        | otherwise = do
            i' <- up i
            j' <- down j
            if i' <= j'
              then do
                a <- unsafeRead values i'
                b <- unsafeRead values j'
                unsafeWrite values i' b
                unsafeWrite values j' a
                go (i' + 1) (j' - 1)
              else go i' j'
  (i, j) <- go low high
  quicksort values low j
  quicksort values i high

sortNumbers :: Ints -> IO Int
sortNumbers seed = do
  numbers <- newArray (0, sortCount - 1) 0 :: IO Ints
  loop 0 sortCount $ \i -> random seed >>= \r -> unsafeWrite numbers i (r `rem` 1000000)
  quicksort numbers 0 (sortCount - 1)
  let check !i !acc
        | i >= sortCount = return acc
        | otherwise = unsafeRead numbers i >>= \v -> check (i + 1000) (acc + v * (i `rem` 7 + 1))
  check 0 0

queensWays :: IO Int
queensWays = do
  columns <- newArray (0, queens - 1) 0 :: IO Bytes
  falling <- newArray (0, 2 * queens - 1) 0 :: IO Bytes
  rising <- newArray (0, 2 * queens - 1) 0 :: IO Bytes
  let place !row
        | row == queens = return 1
        | otherwise = try row 0 0
      try !row !column !ways
        | column >= queens = return (ways :: Int)
        | otherwise = do
            let down = row + column
                up = row - column + queens
            c <- unsafeRead columns column
            f <- unsafeRead falling down
            r <- unsafeRead rising up
            if c /= 0 || f /= 0 || r /= 0
              then try row (column + 1) ways
              else do
                unsafeWrite columns column 1
                unsafeWrite falling down 1
                unsafeWrite rising up 1
                more <- place (row + 1)
                unsafeWrite columns column 0
                unsafeWrite falling down 0
                unsafeWrite rising up 0
                try row (column + 1) (ways + more)
  place 0

hash :: Ints -> IO Int
hash seed = do
  table <- newArray (0, hashSize - 1) 0 :: IO Ints
  let find !key !slot !probes = do
        v <- unsafeRead table slot
        if v /= 0 && v /= key then find key ((slot + 1) `rem` hashSize) (probes + 1) else return (slot, probes)
  loop 0 hashKeys $ \_ -> do
    key <- random seed
    (slot, _) <- find key (key `rem` hashSize) (0 :: Int)
    unsafeWrite table slot key
  unsafeWrite seed 0 42
  let ask !i !found !probes
        | i >= hashKeys = return (found * 100 + probes)
        | otherwise = do
            r <- random seed
            let !key = r + i `rem` 2
            (slot, more) <- find key (key `rem` hashSize) 0
            v <- unsafeRead table slot
            ask (i + 1) (if v == key then found + 1 else found) (probes + more)
  ask 0 0 0

bytes :: Ints -> IO Int
bytes seed = do
  text <- newArray (0, bytesCount - 1) 0 :: IO Bytes
  counts <- newArray (0, 25) 0 :: IO Ints
  loop 0 bytesCount $ \i -> random seed >>= \r -> unsafeWrite text i (fromIntegral (97 + r `rem` 26))
  let scan !i !code
        | i >= bytesCount = return code
        | otherwise = do
            letter <- fromIntegral <$> unsafeRead text i
            n <- unsafeRead counts (letter - 97)
            unsafeWrite counts (letter - 97) (n + 1)
            scan (i + 1) ((code * 31 + letter) `rem` 1000000007)
  code <- scan 0 (0 :: Int)
  let search !i !before !pairs
        | i >= bytesCount = return pairs
        | otherwise = do
            c <- unsafeRead text i
            search (i + 1) c (if before == 119 && c == 100 then pairs + 1 else pairs)
  first <- unsafeRead text 0
  pairs <- search 1 first (0 :: Int)
  es <- unsafeRead counts 4
  return (code + es + pairs * 7)

-- A node is its place in two arrays: one of values, one of the place of the
-- node after it, with -1 for the end of the list.
list :: Ints -> IO Int
list seed = do
  values <- newArray (0, listCount - 1) 0 :: IO Ints
  nexts <- newArray (0, listCount - 1) (-1) :: IO Ints
  order <- newArray (0, listCount - 1) 0 :: IO Ints
  loop 0 listCount $ \i -> unsafeWrite order i i >> unsafeWrite values i (i `rem` 100)
  let shuffle !i = when (i > 0) $ do
        r <- random seed
        let j = r `rem` (i + 1)
        a <- unsafeRead order i
        b <- unsafeRead order j
        unsafeWrite order i b
        unsafeWrite order j a
        shuffle (i - 1)
  shuffle (listCount - 1)
  loop 0 (listCount - 1) $ \i -> do
    a <- unsafeRead order i
    b <- unsafeRead order (i + 1)
    unsafeWrite nexts a b
  first <- unsafeRead order 0
  let walk !pass !at !total
        | at < 0 = return total
        | otherwise = do
            v <- unsafeRead values at
            next <- unsafeRead nexts at
            walk pass next (total + v + pass)
      passes !pass !total
        | pass >= 20 = return (total :: Int)
        | otherwise = walk pass first total >>= passes (pass + 1)
  passes 0 0

main :: IO ()
main = do
  args <- getArgs
  seed <- newArray (0, 0) 42 :: IO Ints
  let kernel = case args of
        (name : _) -> name
        [] -> ""
  result <- case kernel of
    "fib" -> return (fib 38)
    "sieve" -> sieve
    "collatz" -> return collatz
    "gcd" -> return gcdSum
    "mandelbrot" -> return mandelbrot
    "matmul" -> matmul
    "particles" -> particleSum seed
    "sort" -> sortNumbers seed
    "queens" -> queensWays
    "hash" -> hash seed
    "bytes" -> bytes seed
    "list" -> list seed
    _ -> do
      putStrLn ("unknown kernel '" ++ kernel ++ "'")
      exitWith (ExitFailure 2)
  print result
