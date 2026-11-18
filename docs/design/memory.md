# Memory: growable arrays, maps and freeing

Wind gives a program memory that grows without a garbage collector and
without hidden work, in the way Odin does, written in Wind's C-like style.
This note records what was chosen and why, and what is still to come.

## Chosen

| | Choice | Why |
| --- | --- | --- |
| Growable array | `int xs[..]` | C's array syntax, extended; reads like `int xs[5]` and `int xs[]` |
| Map | `int counts[string]` | the same shape, as in the D language |
| Map keys | integers, `char`, `bool`, enums, pointers, strings; a string key is copied into the map | keys compare byte for byte, or as text; floats and structs have no exact equality in Wind |
| Missing key | reads as zero and adds nothing; assigning puts it in, and `m[k] += 1` starts from zero | as in Go and Odin; `has(m, k)` tells the two apart without multiple return values |
| Map order | the order keys were first put | a program's output is the same on every run, as in Python; Go's random order is a source of flaky output |
| Going through | `for (T x in a)`, `for (int i, T x in a)`, `for (K k, V v in m)` | the types are written, as everywhere in Wind; one name is the element or key, two add the index or value |
| Empty value | a growable array starts empty and allocates nothing | the zero value is always valid, as everywhere in Wind |
| Built-ins | `append(xs, v...)`, `pop(xs)`, `clear(xs)`, `reserve(xs, n)`, `delete(xs)`, `clone(a)` | arrays are passed by reference, so the array is named, not `&xs` |
| Passing | a growable array goes to a `T name[]` parameter as a view of its elements; a `T name[..]` parameter is the caller's own array, to grow | functions written for fixed arrays work unchanged |
| Freeing | `delete`, and `defer` to keep it next to the allocation | explicit, as in C and Odin; nothing happens that is not written |
| Never freed | fine: the operating system takes the memory back at exit | scripts need not clean up |
| Where memory comes from | one heap (`malloc`/`realloc`/`free`) | no hidden context parameter in every function, so the generated C stays plain |
| Pointers into elements | `&xs[i]` is an error, and so is using `xs` as a pointer | elements move when the array grows; this rules out the dangling pointer that C and Odin allow. It can be relaxed later without breaking programs |
| Copying | `ys = xs` is an error; `ys = clone(xs)` copies | two arrays never share elements, so one can never free the other's |
| Errors | an index out of range and `pop` on an empty array stop the program in checked builds; running out of memory always does | the same checks, and the same switch, as fixed arrays |
| Use after `delete` | safe: `delete` leaves the array empty, so a later index is out of range and a later `append` starts afresh | follows from the two rules above, at no cost |

## The generated C

A growable array of `T` is a struct `wind_list_T { T *data; size_t len;
size_t cap; }`, defined only when used. Its first two fields are those of
the slice struct an array parameter is, so indexing, `len` and bounds checks
are shared with array parameters. A growable parameter is a pointer to the
caller's struct. `append` grows through one helper, `wind_grow`, which
doubles the room each time; the built-ins are GNU statement expressions.

`defer` is resolved by the compiler: each place a block is left (its end, a
`return`, a `break`, a `continue`) has the deferred statements of the blocks
it leaves written out before it, innermost first. A `return` with a value
works the value out first. A function with a `defer` cannot use `goto`,
which could jump past it.

A map is one C type, `wind_map`, whatever its keys and values: its entries
are kept in arrays in the order they were put, with a hash table of where
they are, and the generated code tells the map functions the sizes of a key
and a value. A removed entry stays in place, marked as gone, until the table
is rebuilt, which keeps the order of the others. Writing `m[k] = v` works
out the key and the value first, and then finds or puts the entry, so the
value never refers to a place the insertion has moved.

## To come

1. **Building strings**: `append(text, "more")` on a `char[..]` to add a
   string's bytes, `string(text)` for a view of them, and formatting into
   one, which gives string concatenation and padded, fixed-precision output.
2. **Scratch memory**: `#temp { ... }`, where everything allocated inside is
   freed as the block ends; it could later name an allocator to use there.
3. **Leak reports**: an optional report, at exit, of growable arrays never
   deleted, for programs that should clean up.
4. **Boards**: a warning, or an opt-in, for growable arrays on boards with a
   few kilobytes of memory, where the heap fragments quickly.
