# The benchmark program in Elixir; see bench.wind for what each kernel
# measures. Elixir's own data cannot be changed in place, so this is written
# for speed with what the language offers instead: Erlang's :atomics for the
# arrays of whole numbers that are written to, tuples for the ones that are
# only read, a list of tuples for the particles, a binary for the bytes, and
# whole numbers used as sets of bits for the queens. The state of the random
# generator is passed along from call to call.
defmodule Bench do
  import Bitwise

  @sieve_limit 20_000_000
  @sort_count 2_000_000
  @matrix_size 400
  @list_count 1_000_000
  @hash_size 2_097_152
  @hash_keys 1_000_000
  @bytes_count 4_000_000
  @particle_count 1000
  @queens 12

  @compile {:inline, random: 1, at: 2, put: 3}

  defp random(seed), do: rem(seed * 48271, 2_147_483_647)

  # An :atomics array counts its places from 1; these count from 0.
  defp at(array, index), do: :atomics.get(array, index + 1)
  defp put(array, index, value), do: :atomics.put(array, index + 1, value)

  defp fib(n) when n < 2, do: n
  defp fib(n), do: fib(n - 1) + fib(n - 2)

  defp sieve do
    composite = :atomics.new(@sieve_limit + 1, signed: true)
    sieve(composite, 2, 0)
  end

  defp sieve(_composite, i, count) when i > @sieve_limit, do: count

  defp sieve(composite, i, count) do
    if at(composite, i) != 0 do
      sieve(composite, i + 1, count)
    else
      mark(composite, i * 2, i)
      sieve(composite, i + 1, count + 1)
    end
  end

  defp mark(_composite, j, _i) when j > @sieve_limit, do: :ok

  defp mark(composite, j, i) do
    put(composite, j, 1)
    mark(composite, j + i, i)
  end

  defp collatz(start, best, best_start) when start >= 300_000, do: best_start * 1000 + best

  defp collatz(start, best, best_start) do
    steps = chain(start, 0)
    if steps > best, do: collatz(start + 1, steps, start), else: collatz(start + 1, best, best_start)
  end

  defp chain(1, steps), do: steps
  defp chain(n, steps) when band(n, 1) == 0, do: chain(bsr(n, 1), steps + 1)
  defp chain(n, steps), do: chain(3 * n + 1, steps + 1)

  defp gcd(x, 0), do: x
  defp gcd(x, y), do: gcd(y, rem(x, y))

  defp gcd_sum(i, _j, total) when i > 1200, do: total
  defp gcd_sum(i, j, total) when j > 1200, do: gcd_sum(i + 1, 1, total)
  defp gcd_sum(i, j, total), do: gcd_sum(i, j + 1, total + gcd(i, j))

  defp mandelbrot(py, _px, total) when py >= 600, do: total
  defp mandelbrot(py, px, total) when px >= 600, do: mandelbrot(py + 1, 0, total)

  defp mandelbrot(py, px, total) do
    cx = px * 3.0 / 600.0 - 2.0
    cy = py * 2.0 / 600.0 - 1.0
    mandelbrot(py, px + 1, total + escape(cx, cy, 0.0, 0.0, 0))
  end

  defp escape(cx, cy, x, y, i) when i < 200 and x * x + y * y <= 4.0 do
    escape(cx, cy, x * x - y * y + cx, 2.0 * x * y + cy, i + 1)
  end

  defp escape(_cx, _cy, _x, _y, i), do: i

  # Each product is added to the total as it is made, in the order the other
  # languages add them up afterwards.
  defp matmul do
    n = @matrix_size
    left = List.to_tuple(for i <- 0..(n * n - 1), do: rem(i, 7) + 0.5)
    right = List.to_tuple(for i <- 0..(n * n - 1), do: rem(i, 5) - 1.5)
    trunc(matmul(left, right, 0, 0, 0.0))
  end

  defp matmul(_left, _right, i, _j, total) when i >= @matrix_size, do: total
  defp matmul(left, right, i, j, total) when j >= @matrix_size, do: matmul(left, right, i + 1, 0, total)

  defp matmul(left, right, i, j, total) do
    matmul(left, right, i, j + 1, total + dot(left, right, i * @matrix_size, j, 0, 0.0))
  end

  defp dot(_left, _right, _row, _j, k, sum) when k >= @matrix_size, do: sum

  defp dot(left, right, row, j, k, sum) do
    dot(left, right, row, j, k + 1, sum + elem(left, row + k) * elem(right, k * @matrix_size + j))
  end

  defp particle_sum(seed) do
    particles = make_particles(@particle_count, seed)
    moved = steps(particles, 20_000)
    trunc(Enum.reduce(moved, 0.0, fn {x, y, _vx, _vy}, total -> total + (x + y) end))
  end

  defp make_particles(0, _seed), do: []

  defp make_particles(count, seed) do
    a = random(seed)
    b = random(a)
    c = random(b)
    d = random(c)
    particle = {rem(a, 1000) * 1.0, rem(b, 1000) * 1.0, (rem(c, 200) - 100) / 8.0, (rem(d, 200) - 100) / 8.0}
    [particle | make_particles(count - 1, d)]
  end

  defp steps(particles, 0), do: particles
  defp steps(particles, left), do: steps(move(particles), left - 1)

  defp move([]), do: []

  defp move([{x, y, vx, vy} | rest]) do
    x = x + vx * 0.5
    y = y + vy * 0.5
    vx = if x < 0.0 or x > 1000.0, do: -vx, else: vx
    vy = if y < 0.0 or y > 1000.0, do: -vy, else: vy
    [{x, y, vx, vy} | move(rest)]
  end

  defp sort(seed) do
    numbers = :atomics.new(@sort_count, signed: true)
    fill_numbers(numbers, 0, seed)
    quicksort(numbers, 0, @sort_count - 1)
    check(numbers, 0, 0)
  end

  defp fill_numbers(_numbers, i, _seed) when i >= @sort_count, do: :ok

  defp fill_numbers(numbers, i, seed) do
    seed = random(seed)
    put(numbers, i, rem(seed, 1_000_000))
    fill_numbers(numbers, i + 1, seed)
  end

  defp quicksort(_values, low, high) when low >= high, do: :ok

  defp quicksort(values, low, high) do
    pivot = at(values, div(low + high, 2))
    {i, j} = partition(values, pivot, low, high)
    quicksort(values, low, j)
    quicksort(values, i, high)
  end

  defp partition(_values, _pivot, i, j) when i > j, do: {i, j}

  defp partition(values, pivot, i, j) do
    i = up(values, pivot, i)
    j = down(values, pivot, j)

    if i <= j do
      held = at(values, i)
      put(values, i, at(values, j))
      put(values, j, held)
      partition(values, pivot, i + 1, j - 1)
    else
      partition(values, pivot, i, j)
    end
  end

  defp up(values, pivot, i), do: if(at(values, i) < pivot, do: up(values, pivot, i + 1), else: i)
  defp down(values, pivot, j), do: if(at(values, j) > pivot, do: down(values, pivot, j - 1), else: j)

  defp check(_numbers, i, total) when i >= @sort_count, do: total
  defp check(numbers, i, total), do: check(numbers, i + 1000, total + at(numbers, i) * (rem(i, 7) + 1))

  # The three arrays of the other languages are three whole numbers here,
  # with a bit for each column and each diagonal.
  defp place(@queens, _columns, _falling, _rising), do: 1
  defp place(row, columns, falling, rising), do: try_columns(row, 0, columns, falling, rising, 0)

  defp try_columns(_row, @queens, _columns, _falling, _rising, ways), do: ways

  defp try_columns(row, column, columns, falling, rising, ways) do
    c = bsl(1, column)
    f = bsl(1, row + column)
    r = bsl(1, row - column + @queens)

    if band(columns, c) != 0 or band(falling, f) != 0 or band(rising, r) != 0 do
      try_columns(row, column + 1, columns, falling, rising, ways)
    else
      more = place(row + 1, bor(columns, c), bor(falling, f), bor(rising, r))
      try_columns(row, column + 1, columns, falling, rising, ways + more)
    end
  end

  defp hash(seed) do
    table = :atomics.new(@hash_size, signed: true)
    fill_table(table, 0, seed)
    ask(table, 0, seed, 0, 0)
  end

  defp fill_table(_table, i, _seed) when i >= @hash_keys, do: :ok

  defp fill_table(table, i, seed) do
    key = random(seed)
    {slot, _probes} = find(table, key, rem(key, @hash_size), 0)
    put(table, slot, key)
    fill_table(table, i + 1, key)
  end

  defp find(table, key, slot, probes) do
    held = at(table, slot)
    if held != 0 and held != key, do: find(table, key, rem(slot + 1, @hash_size), probes + 1), else: {slot, probes}
  end

  defp ask(_table, i, _seed, found, probes) when i >= @hash_keys, do: found * 100 + probes

  defp ask(table, i, seed, found, probes) do
    seed = random(seed)
    key = seed + rem(i, 2)
    {slot, more} = find(table, key, rem(key, @hash_size), 0)
    found = if at(table, slot) == key, do: found + 1, else: found
    ask(table, i + 1, seed, found, probes + more)
  end

  defp bytes(seed) do
    text = make_text(0, seed, <<>>)
    counts = :atomics.new(26, signed: true)
    code = scan(text, counts, 0)
    <<first, rest::binary>> = text
    code + at(counts, 4) + pairs(rest, first, 0) * 7
  end

  defp make_text(i, _seed, text) when i >= @bytes_count, do: text

  defp make_text(i, seed, text) do
    seed = random(seed)
    make_text(i + 1, seed, <<text::binary, 97 + rem(seed, 26)>>)
  end

  defp scan(<<>>, _counts, code), do: code

  defp scan(<<letter, rest::binary>>, counts, code) do
    :atomics.add(counts, letter - 96, 1)
    scan(rest, counts, rem(code * 31 + letter, 1_000_000_007))
  end

  defp pairs(<<>>, _before, count), do: count
  defp pairs(<<?d, rest::binary>>, ?w, count), do: pairs(rest, ?d, count + 1)
  defp pairs(<<letter, rest::binary>>, _before, count), do: pairs(rest, letter, count)

  # A node is its place in two arrays: one of values, one of the place of
  # the node after it, with -1 for the end of the list.
  defp list(seed) do
    values = :atomics.new(@list_count, signed: true)
    nexts = :atomics.new(@list_count, signed: true)
    order = :atomics.new(@list_count, signed: true)
    number(values, order, 0)
    shuffle(order, @list_count - 1, seed)
    link(nexts, order, 0)
    put(nexts, at(order, @list_count - 1), -1)
    passes(values, nexts, at(order, 0), 0, 0)
  end

  defp number(_values, _order, i) when i >= @list_count, do: :ok

  defp number(values, order, i) do
    put(order, i, i)
    put(values, i, rem(i, 100))
    number(values, order, i + 1)
  end

  defp shuffle(_order, 0, _seed), do: :ok

  defp shuffle(order, i, seed) do
    seed = random(seed)
    j = rem(seed, i + 1)
    held = at(order, i)
    put(order, i, at(order, j))
    put(order, j, held)
    shuffle(order, i - 1, seed)
  end

  defp link(_nexts, _order, i) when i >= @list_count - 1, do: :ok

  defp link(nexts, order, i) do
    put(nexts, at(order, i), at(order, i + 1))
    link(nexts, order, i + 1)
  end

  defp passes(_values, _nexts, _first, 20, total), do: total

  defp passes(values, nexts, first, pass, total) do
    passes(values, nexts, first, pass + 1, walk(values, nexts, first, pass, total))
  end

  defp walk(_values, _nexts, -1, _pass, total), do: total

  defp walk(values, nexts, place, pass, total) do
    walk(values, nexts, at(nexts, place), pass, total + at(values, place) + pass)
  end

  def main(args) do
    kernel = List.first(args) || ""

    result =
      case kernel do
        "fib" -> fib(38)
        "sieve" -> sieve()
        "collatz" -> collatz(1, 0, 0)
        "gcd" -> gcd_sum(1, 1, 0)
        "mandelbrot" -> mandelbrot(0, 0, 0)
        "matmul" -> matmul()
        "particles" -> particle_sum(42)
        "sort" -> sort(42)
        "queens" -> place(0, 0, 0, 0)
        "hash" -> hash(42)
        "bytes" -> bytes(42)
        "list" -> list(42)
        _ ->
          IO.puts("unknown kernel '#{kernel}'")
          System.halt(2)
      end

    IO.puts(result)
  end
end
