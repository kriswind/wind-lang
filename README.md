# Wind

A small, explicit programming language in the spirit of Odin. The compiler is
written in C11 and translates Wind to C, then hands the result to the system C
compiler.

```wind
const string Greeting = "Hello"

int add(int a, int b) {
    return a + b
}

for (int i = 0; i < 3; i++) {
    cout << Greeting << " " << add(i, 1) << "\n"
}
```

## Build and use

```sh
git clone https://github.com/kriswind/wind-lang.git
cd wind-lang

make            # produces build/wind
make test

build/wind examples/hello.wind              # compile and run
build/wind run examples/hello.wind          # the same, spelled out
build/wind build examples/hello.wind -o hi  # compile to an executable
build/wind build --no-bounds-check examples/hello.wind   # without index checks
build/wind check examples/hello.wind        # report errors and warnings only
build/wind emit examples/hello.wind         # print the generated C
build/wind build -t arduino examples/arduino/blink.wind  # an Arduino sketch
build/wind emit -t arduino examples/arduino/blink.wind   # print the sketch's C
build/wind lsp                              # language server on stdio

make install    # installs wind to ~/.local/bin and its libraries to
                # ~/.local/share/wind/libs (override with PREFIX=)
```

Requires GCC or Clang on a POSIX system. `$CC` selects the C compiler used
both for building `wind` and by `wind` itself (default `cc`).

`make test` also needs `python3`, which drives the language server test.

## Libraries

`libs/` holds the libraries that come with Wind. An import whose path starts
with `::` is found there, so `import ::vendor/arduino` is the folder
`libs/vendor/arduino`, a [module made of several files](#modules). The
compiler looks for the directory beside itself: `libs` in a build of the
source, `share/wind/libs` once installed. `WIND_LIBS` names another one.

| Library | What it holds |
| --- | --- |
| `::math` | C's maths functions (`sqrt`, `pow`, `sin`, `atan2`, `floor`, `round`…), `PI` and `E`, and helpers: `min`, `max`, `clamp`, `abs`, `sign`, `gcd`, `lcm`, `is_prime`, `lerp`, `radians`, `degrees` |
| `::random` | Pseudo-random numbers that a seed makes repeatable on any machine or board: `seed`, `next`, `range`, `unit`, `chance`, `shuffle` |
| `::text` | Characters and strings: `is_digit`, `is_letter`, `to_upper`…, `index_of`, `contains`, `starts_with`, `ends_with`, `count`, and `parse_int`. A folder of three files; `::text/chars` and the others can be imported alone |
| `::vendor/arduino` | The Arduino core: pins, time, the serial port and the board's constants; see [The Arduino toolchain](#the-arduino-toolchain) |

`::math`, `::random` and `::text` work on every target. Examples that use
them are in `examples/libraries`, and `examples/arduino/dice.wind` uses
`::random` and `::math` on a board.

## Tooling

The linter and language server are built into the `wind` binary and share the
compiler's own lexer, parser and checker, so an editor reports exactly what a
build would.

- `wind check <file>...` prints every diagnostic as
  `file:line:col: severity: message` and exits with status 1 if there are
  errors. Warnings do not fail a check or a build. They cover an unused
  constant, variable, function, label or import, and unreachable code.
  Problems in an imported file are listed under that file's own name. A
  program for a board, by its `#target` or `-t arduino`, is checked with
  the board's sizes and rules, in the editor too.
- `wind lsp` provides diagnostics as you type, hover, go-to-definition and
  completion for constants, variables, functions, parameters, labels and
  modules. Go-to-definition follows a name into the file that exports it, and
  typing `module.` lists what the module exports. Imported files are read
  from disk, so save a file for the files that import it to see the change.

Code is reported as unreachable only where that is certain: after a
`return`, `break`, `continue` or `goto`, after a loop that never ends, and in
a body whose condition is the literal `true` or `false`. A condition that is a
named constant is not reported, so `const bool Debug = false` can switch code
off quietly, and neither is code that a `goto` can reach through a label.
Editors show unreachable code faded.

Setup for Neovim (including Mason), Sublime Text and Helix is in
[editors/README.md](editors/README.md).

## The language

A file can be laid out in one of two ways:

- **As a script.** Top-level statements are the program and run in order.
- **As a C-style program.** If the file defines a function named `main`, that
  is the entry point, and the top level may only hold declarations:
  constants, variables and functions.

```wind
int runs = 0

int main() {
    runs = runs + 1
    cout << "runs: " << runs << "\n"
    return 0
}
```

`main` returns `int` (the process exit status) or `void`. It takes no
parameters, or the program's command-line arguments as
`main(int argc, string argv[])`, where `argv[0]` is the program itself:

```sh
wind program.wind one two     # argc is 3
```
 Top-level variable initializers run, in order, before `main`.

### Basics

- A statement ends at a newline or a `;`. Semicolons are optional.
- A newline does not end a statement directly after `=`, `<<`, `>>`, `,` or a
  binary operator, or inside parentheses. A line that starts with a binary
  operator, `.`, `->`, the `?` or `:` of a conditional, or the `<<` or `>>`
  of a `cout` or `cin`, carries on the line before:

  ```wind
  int total = price
      + tax
      - discount
  cout << "total: " << total
       << "\n"
  ```

  `*` and `&` can also start a statement (`*p = 7`), so a line that starts
  with one of them carries on the line before only when a space follows it,
  as in `* scale`.
- `// ...` is a comment to the end of the line; `/* ... */` is a comment that
  may span lines. Block comments do not nest.
- Names are `[A-Za-z_][A-Za-z0-9_]*`. Keywords and type names are reserved.

### Types

| Kind | Types |
| --- | --- |
| Signed integers | `int`, `i8`, `i16`, `i32`, `i64`, `i128` |
| Unsigned integers | `uint`, `u8`, `u16`, `u32`, `u64`, `u128`, `uintptr` |
| Floats | `float`, `f16`, `f32`, `f64`, `f128` |
| Text | `string`, `char` |
| Other | `bool`, `void` (function return type only) |
| Pointers | `T *` for any type `T`; see [Pointers](#pointers) |

- `int`, `uint` and `uintptr` are pointer-sized. `float` is 64-bit.
- `byte` is another name for `u8`.
- A `string` is a contiguous array of bytes with an explicit length (a
  pointer and a length, as in Odin). It is not NUL-terminated, so it may
  contain any byte, including `\0`.
- A `char` is one byte: the element type of a string.

C's type names are accepted as other names for these types:

| C name | Wind type |
| --- | --- |
| `short`, `long`, `long long` (optionally followed by `int`) | `i16`, `i64`, `i64` |
| `unsigned short`, `unsigned long`, `unsigned long long` | `u16`, `u64`, `u64` |
| `signed`, `unsigned`, `unsigned int` | `int`, `uint`, `uint` |
| `signed char`, `unsigned char` | `i8`, `u8` |
| `double`, `long double` | `f64`, `f128` |
| `int8_t` … `int64_t`, `uint8_t` … `uint64_t` | `i8` … `i64`, `u8` … `u64` |
| `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t` | `uint`, `int`, `int`, `int`, `uintptr` |

Two names keep their Wind meaning and so differ from C: `int` is
pointer-sized (64-bit, not 32), and `float` is 64-bit (not 32).

There are no implicit conversions. Both sides of an operator must have the
same type, as must a value and the variable, parameter or return type it goes
to. Convert explicitly with `T(x)`:

```wind
i64 wide = 40
i8 narrow = i8(wide) + 2
float ratio = float(narrow) / 4.0
char letter = char(65)
```

Conversions work between any two numeric types and between `char` and
integers. C's spelling works too: `(i8)wide` is `i8(wide)`. As in C it applies
to the one value after it, so `(float)a / b` converts `a` and then divides.

### Literals

| Literal | Examples |
| --- | --- |
| Integer | `42`, `1_000_000`, `0xFF`, `0b1010`, `0o17` |
| Float | `1.5`, `2e10`, `6.02e+23` |
| Character | `'a'`, `'\n'` |
| String | `"text"` |
| Boolean | `true`, `false` |

Number literals have no type of their own. They take the type the context
needs, so `u8 x = 200` and `f32 y = 1` both work, and default to `int` or
`float` otherwise. A literal that does not fit its type is an error. C's
literal suffixes (`1.5f`, `10UL`, `2.0L`) are accepted and ignored.

`sizeof(type)` and `sizeof expr` give a size in bytes and behave like an
integer literal, so the result fits whatever integer type it is used as.

Escapes in strings and characters: `\n` `\t` `\r` `\0` `\\` `\"` `\'`.
A string literal cannot span lines.

### Variables and constants

```wind
int count = 3        // variable
string name          // variable with the zero value ("" here)
const f64 Pi = 3.14  // constant
count = count + 1
count += 2           // also -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
count++              // also count--, ++count, --count
```

- `++` and `--` are statements, not expressions: they cannot be used inside a
  larger expression.
- A variable without an initializer starts at its type's zero value: `0`,
  `false` or `""`.
- A constant's value must be known at compile time: literals, other
  constants, and operators over them. It cannot be assigned to.
- `const { ... }` declares several constants without repeating `const`.
  Each line in it is a typed declaration with a value, and the names are
  declared where the block is, not inside a scope of their own. It can go
  wherever a constant can, and `export const { ... }` exports them all:

  ```wind
  const {
      int ROWS = 9
      int COLUMNS = 9
      int CELLS = ROWS * COLUMNS
      string TITLE = "sudoku"
  }
  ```
- Names must be declared before use. A name may shadow one from an enclosing
  block, but cannot be declared twice in the same block.

### Arrays

An array holds a fixed number of elements of one type. Its length is set
when it is declared and never changes.

```wind
int scores[5] = {12, 47, 3}   // the remaining elements are zero
string names[2]               // all elements are zero ("" here)
u8 bytes[] = {1, 2, 3}        // length taken from the elements: 3

scores[4] = scores[0] + 1
scores[1] += 10
cout << len(scores) << "\n"   // 5
```

- A length must be known at compile time: a literal, a constant, or
  arithmetic over them.
- Elements are indexed from zero. A negative index known at compile time
  counts back from the end, as in Python: `scores[-1]` is the last element
  and `scores[-2]` the one before it. The compiler turns it into the
  position it names, so it costs nothing when the program runs. It needs a
  length known at compile time too: a fixed array, a string literal or a
  string constant. For an array parameter or a string variable, write
  `values[len(values) - 1]`.
- An index worked out while the program runs is never counted back from
  the end: a negative one is out of range, so a mistake such as
  `grid[cell - width]` on the top row is caught rather than reading the
  bottom row.
- Every index is checked: one that is out of range, in either direction,
  stops the program with a message naming the line, the index and the
  length. An index known at compile time that is out of range is a
  compile-time error.
  Building with `--no-bounds-check` leaves the run-time test out; an index
  that is out of range is then undefined, as in C.
- `#no_bounds_check` and `#bounds_check` set this for one piece of code,
  whatever the build says; see below.
- `len(x)` gives the number of elements of an array, or of bytes in a
  string, as an `int`. `sizeof(array)` gives its size in bytes, so C's
  `sizeof(a) / sizeof(a[0])` works too.
- `len` of a fixed array, a string literal or a string constant is known at
  compile time, so it can give a constant its value or an array its length:
  `const int count = len(pets)`.
- A string can be indexed the same way: `word[0]` is a `char`. Strings cannot
  be changed, so a character cannot be assigned.
- `const int Primes[3] = {2, 3, 5}` declares an array whose elements cannot
  be changed.
- `cout << scores` prints the whole array as `[12, 57, 3, 0, 13]`. Inside an
  array, strings and chars are quoted and escaped (`["a", "b"]`, `['x']`) so
  that the elements can be told apart.
- An array declared inside a function lives on the stack, which is small:
  usually 8 MB on a computer and a few kilobytes on a board. One over 1 MB
  (1 KB on a board) gets a warning, since running out of stack ends the
  program with no message. Declare a large array at the top level, outside
  any function, where it can be as large as memory allows.

A function takes an array with a parameter written `T name[]`. It receives a
view of the caller's array, not a copy, together with its length, so it works
for arrays of any length and can change their elements:

```wind
void clear(int values[]) {
    for (int i = 0; i < len(values); i++) {
        values[i] = 0
    }
}
```

**Bounds checks in one place**

```wind
#no_bounds_check
int sum(int values[]) {           // the whole function
    int total = 0
    for (int i = 0; i < len(values); i++) total += values[i]
    return total
}

#no_bounds_check {                // a block
    for (int i = 0; i < 5; i++) squares[i] = i * i
    #bounds_check squares[n] = 0  // one statement, checked again
}

#no_bounds_check for (int i = 0; i < 5; i++) total += squares[i]
```

- `#no_bounds_check` leaves out the run-time test for every index in the
  statement, block or function that follows it; `#bounds_check` puts it
  back, also in a program built with `--no-bounds-check`.
- The directive can sit on the line above what it covers. The innermost
  one wins. It covers the code written there, not the functions that code
  calls.
- An index known at compile time is settled either way: a negative one
  counts back from the end, and one out of range is still a compile-time
  error.
- `NULL` checks on pointers are not affected.

An array is not a value in itself. Its name can be indexed, measured,
printed, and passed to an array parameter, but one array cannot be assigned
to another, compared with `==`, or returned from a function. Used any other
way, the name is a pointer to the array's first element; see
[Pointers](#pointers). Arrays have one dimension, and their elements cannot
themselves be arrays.

### Operators

| Operators | Operands | Result |
| --- | --- | --- |
| `+` `-` `*` `/` | numbers | same type |
| `%` | integers | same type |
| unary `-` | numbers | same type |
| `==` `!=` | any two values of one type | `bool` |
| `<` `<=` `>` `>=` | numbers, `char` | `bool` |
| `&&` `\|\|` `!` | `bool`, or a pointer (true unless `NULL`) | `bool` |
| `&` `\|` `^` (and, or, xor) | integers | same type |
| unary `~` (not) | integers | same type |
| unary `&` `*`, and `+` `-` on pointers | see [Pointers](#pointers) | |
| `<<` `>>` (shifts) | an integer, shifted by any integer | type of the value shifted |

| `c ? a : b` (conditional) | a `bool`, then two values of one type | that type |

From tightest to loosest: unary and casts, `* / %`, `+ -`, `<< >>`, `&`, `^`,
`|`, comparisons, `&&`, `||`, `?:`. Integer arithmetic wraps at the width of
its type.

Operands are evaluated from left to right, and so are the arguments of a
call and the values between braces: `show(next(), next())` passes the first
number drawn, then the second, whatever C compiler builds the program. An
assignment finds its place before it evaluates its value, so
`slots[i] = advance()` stores into the element `i` named before the call.

`c ? a : b` is `a` when `c` is true and `b` otherwise; only the chosen one is
evaluated. Conditionals chain to the right, so `a ? x : b ? y : z` reads as a
list of cases.

The bitwise operators differ from C in three ways:

- **They bind tighter than comparisons.** `x & 1 == 0` tests whether the
  masked value is zero; in C it would mean `x & (1 == 0)`.
- **Narrow types are not widened.** `u8 << 1` is a `u8`, and bits shifted
  past its width are discarded.
- **Every shift is defined.** `>>` copies the sign bit for signed types and
  shifts in zeros for unsigned ones. Shifting by a negative amount, or by
  the width or more, shifts every bit out: the result is 0, or -1 for a
  negative signed value shifted right.

In a `cout` statement `<<` separates the values, so a shift there goes in
parentheses: `cout << (flags << 2)`. A conditional needs none:
`cout << n == 1 ? "item" : "items" << "\n"`.

### Control flow

```wind
if count > 10 {
    cout << "many\n"
} else if count > 0 {
    cout << "some\n"
} else {
    cout << "none\n"
}

while count > 0 {
    count--
}

do {
    count += 3
} while count < 10

for (int i = 0; i < count; i++) {
    if i == 2 { continue }
    if i == 7 { break }
    cout << i << "\n"
}
```

- A condition must be a `bool`. Parentheses around it are optional:
  `if (count > 10) {` and `if count > 10 {` are the same, and likewise for
  `while`.
- A body of one statement can go without braces, as in C, when its condition
  is in parentheses: `if (count > 10) count = 10`, `while (n > 0) n--`,
  `for (int i = 0; i < 3; i++) total += i`. So can the body after `else` or
  `do`. The statement may be on the next line, and may not be a declaration
  or a label. An `else` belongs to the nearest `if`. An empty body is `{}`.
- `while` tests its condition before each pass; `do ... while` tests it
  after, so its body runs at least once.
- `for (first; condition; last)` runs `first` once, then repeats the body
  and `last` while the condition holds. `first` is a declaration, an
  assignment or a call, and `last` an assignment or a call. Any of the three
  can be left out; without a condition the loop runs until something leaves
  it. The parentheses are optional. A variable declared in `first` belongs to
  the loop.
- `break` leaves the innermost loop. `continue` skips to its next pass: to
  the condition of a `while` or `do`, or to the last clause of a `for`.
- `name:` declares a label and `goto name` jumps to it. Labels work as in C:
  a label is known throughout the function it is in (or throughout the
  top-level code of a script), so a `goto` can jump forwards or backwards,
  and into or out of blocks.

### Functions

```wind
int square(int n) {
    return n * n
}

void report(string label, int value) {
    cout << label << ": " << value << "\n"
}

report("square", square(7))
```

- Functions are declared at the top level and may be called before their
  declaration, so they can be mutually recursive.
- An empty parameter list can be written `()` or, as in C, `(void)`.
- A function that returns a value must `return` on every path.
- Parameters cannot be assigned to (the elements of an array parameter can).
- A function sees every other function, and the constants and variables
  declared directly at the top level above it.

### Structs, enums and type names

```wind
struct Point { int x; int y }

enum Color { RED, GREEN, BLUE = 10 }

typedef int Meters

Point shift(Point p, int by) { return {p.x + by, p.y + by} }

Point a = {1, 2}
Point b = {.y = 5}            // x is 0
a.x += 10
Point moved = shift(a, 3)
Color c = GREEN
typeof(a.x) n = 7             // n is an int

cout << moved << " " << c << "\n"   // Point{x = 14, y = 5} GREEN
```

**Structs**

- Fields are separated by `;`, `,` or a newline. A field can be of any type,
  including another struct, but not an array.
- A struct is a value, as in C: assigning it, passing it or returning it
  copies it. A variable declared without a value has every field zero.
- `{ ... }` is a struct value. It takes its type from where it is used: a
  declaration, an assignment, an argument, a `return`. Values are given in
  field order, or by name as `.field = value`; fields left out are zero.
  Where nothing gives the type, write it first: `(Point){1, 2}`.
- `value.field` reads a field and `place.field = value` sets one, through
  arrays and nested structs: `points[2].at.x = 5`.
- Structs cannot be compared with `==`; compare their fields.
- `cout` prints a struct as `Name{field = value, ...}`.
- `sizeof` gives the size C would lay the struct out in.

**Enums**

- An enum is a type of its own. Its names are constants of that type, used
  bare as in C: `Color c = RED`.
- Names count up from 0, or from one past the name before; `NAME = value`
  sets one, and the value must be an integer known at compile time.
- An enum converts to and from integers only explicitly: `int(c)`,
  `Color(2)`. So `Color c = 1` and `c + 1` are errors.
- Enum values compare with `==`, `!=`, `<`, `<=`, `>` and `>=`.
- `cout` prints an enum value as its name, or as its number if no name has
  that value. An enum is stored in 32 bits.

**typedef and typeof**

- `typedef type Name` gives a type another name. It does not make a new
  type: a `Meters` is an `int`.
- C's forms work: `typedef struct { ... } Name`, `typedef enum { ... } Name`,
  and `struct Name` or `enum Name` wherever a type is written.
- `typeof(expression)` is the type the expression has, and `typeof(type)` is
  that type. It can be used anywhere a type is written. The expression is
  checked but never run.

**Rules for all three**

- Types are declared at the top level of a file and can be used anywhere in
  it. A type that holds a struct by value must be declared below that
  struct; a pointer can point to a struct declared anywhere, its own
  included.
- `Name(value)` and `(Name)value` convert to a declared type, as they do for
  built-in ones.
- `export` works on `struct`, `enum` and `typedef`. An importing file writes
  `module.Point`, and an exported enum's names as `module.RED`.

### Pointers

```wind
struct Node { int value; Node *next }

void swap(int *a, int *b) {
    int t = *a
    *a = *b
    *b = t
}

int a = 1
int b = 2
swap(&a, &b)

int *p = &a
*p += 10                      // a is 12

Node second = {2, NULL}
Node first = {1, &second}
Node *at = &first
while (at != NULL) {
    cout << at->value << " "  // 1 2
    at = at->next
}

int nums[4] = {1, 2, 3, 4}
int *n = nums                 // points to nums[0]
n += 2
cout << *n << " " << n[1] << " " << n - &nums[0] << "\n"   // 3 4 2
```

- `T *` is a pointer to a `T`, and `T **` a pointer to that. It can be the
  type of a variable, a parameter, a return value, a field or an array's
  elements.
- `&place` is a pointer to a variable, an array element, a field, or what
  another pointer points to. Constants and parameters have no address.
- `*p` is what `p` points to, to read or to assign. `p->field` reaches a
  field through a pointer to a struct, and so does `p.field`.
- A pointer declared without a value is `NULL`. `NULL` takes its type from
  where it is used, so it needs a pointer beside it: `p == NULL`,
  `Node *n = NULL`.
- Using `*`, `->` or `[]` on a `NULL` pointer stops the program with the line
  it happened on.
- A pointer moves by whole elements, as in C: `p + n`, `p - n`, `p++`,
  `p += n`, and `p[n]` for `*(p + n)`. `p - q` is how many elements apart two
  pointers of one type are, as an `int`. None of this is checked: moving a
  pointer outside the array it points into, or keeping one to a variable
  whose function has returned, is undefined, as in C. Returning a pointer
  to one of the function's own variables is reported as a warning.
- An array's name used as a value is a pointer to its first element, as in
  C: `int *n = nums`, `*(nums + 1)`, `p < nums + 4`. A constant array's name
  is not, since the pointer could change it. Comparing two arrays by name is
  an error, because it would compare where they are and not what they hold.
- Pointers of one type compare with `==`, `!=`, `<`, `<=`, `>` and `>=`.
- A pointer can be tested directly, as in C: `if (p)`, `while (at)`, `!p`,
  `p && p->next`, `p ? a : b`. It counts as true unless it is `NULL`. It is
  still not a `bool`: `bool ok = p` is an error; write `p != NULL`.
- A pointer converts only explicitly: to another pointer type, `(u8 *)p`,
  and to and from `uintptr`.
- `cout` prints a pointer as its address, or `NULL`; a `char *` prints as
  text, as described below.
- `*p++` is an error, because C reads it as moving `p`. Write `(*p)++` to
  change what `p` points to, or `p++` to move it.
- A body without braces can start with `*`: `if (ready) *p = 1`.

**`char *` and `string`**

```wind
char *s = "hello"             // a literal, ending in '\0'
int n = 0
while (s[n] != '\0') n++      // 5
cout << s << "\n"             // hello

string view = string(s)       // the same characters, with a length
char *copy = cstring(view)    // a fresh copy, ending in '\0'
copy[0] = 'J'
cout << copy << "\n"          // Jello
```

- A `char *` is C's kind of string: a pointer to characters that end at a
  `'\0'`. A `string` carries its length instead. They are different types,
  and nothing converts between them silently.
- A string literal is a `char *` where one is expected: a declaration, an
  assignment, an argument, a `return`, a field. Its characters cannot be
  changed; writing through the pointer is undefined, as in C.
- `cout` prints a `char *` as the text up to the `'\0'`, so it must point at
  characters that end in one. Inside an array or a struct it is quoted, as a
  `string` is.
- `string(p)` is the characters `p` points to, up to the `'\0'`, as a
  `string`. It is a view, not a copy: it is only good while those characters
  are. `string(NULL)` is empty.
- `cstring(s)` copies a `string` into new memory with a `'\0'` after it and
  gives a `char *` to it. The copy is yours to change, and keeps its memory
  until the program exits.
- A `string`'s own characters cannot be pointed to, since a `string` has no
  `'\0'` and cannot be changed.

### Modules

Every file is a module. `import` makes another file's exports available
under a name, and `export` marks what a file offers:

```wind
// shapes.wind
export const int SIDES = 4
export int area(int w, int h) { return clamp(w * h) }

int clamp(int n) { return n > 1000 ? 1000 : n }   // private to this file
```

```wind
// main.wind
import shapes
import lib/my-text as text
import ::vendor/arduino

cout << shapes.area(3, 4) << " " << shapes.SIDES << "\n"
```

- A path is relative to the file that contains the import. One that starts
  with `::` is in the [libraries](#libraries) directory instead.
- `.wind` can be left off. A path is written as it is, up to the first
  space, or in quotes: `import "lib/my text.wind"`.
- A path that names a folder imports every `.wind` file in it, and in the
  folders below it, as one module. The files are equal parts of it: what
  any of them exports, the module exports, so `import ::vendor/arduino`
  gives both `arduino.pinMode` from `core.wind` and `arduino.waitForSerial`
  from `serial.wind`. Two parts cannot export the same name. A path to one
  of the files, or to a folder below, imports just that:
  `import ::vendor/arduino/serial`. A path is a file before it is a folder;
  one that ends in `/` is always a folder.
- A file's module is named after the file, without `.wind`, and a folder's
  after the folder, unless its files' `#module` names it otherwise; `as
  name` gives either another name. A path whose last part is not a valid
  name, such as `my-text.wind`, needs `as`.
- What a module exports is used as `module.name`. Names never leak into the
  importing file, so two modules can export the same name.
- Functions, constants and types can be exported. Everything else in a file
  is private to it, including its top-level variables, which its functions can
  use and which are initialized before the program starts.
- `export { ... }` exports every declaration in the block, extern blocks
  and `const` blocks included, so a file can gather what it offers in one
  place:

  ```wind
  export {
      const int SIDES = 4
      struct Box { int w; int h }
      int area(Box b) { return b.w * b.h }
  }
  ```
- A file is loaded once, however many files import it, so its variables
  exist once. Imports cannot form a cycle.
- Imports and `export` go at the top level. An import can be used anywhere
  in the file, including above it.
- An imported file is not run as a script: it holds declarations only. It may
  have a `main`, which is ignored when the file is imported, so a module can
  also be run on its own.
- The program is compiled as a whole: `wind build main.wind` follows the
  imports and produces one executable.

### Settings

A file can say what it is compiled for at its very top, above everything
else:

```wind
#target arduino
#board arduino:avr:uno
#module blinker
```

- `#target` names the target: `c`, the machine `wind` runs on, or
  `arduino`; see [The Arduino toolchain](#the-arduino-toolchain). Without
  it, a program is for `c`.
- `#board` names the board, for a target whose programs run on one, as
  `arduino-cli` spells it. The name runs up to the first space; it may
  also be written in quotes.
- `run`, `build`, `check`, `emit` and the language server all read them;
  `-t` and `--fqbn` on the command line override them.
- Only the program's own file chooses. A file it imports can state the
  target it is written for, as `::vendor/arduino` does, and importing it
  into a program for another target is then an error.
- `#module name` names the module that a folder makes, for its imports,
  when that should not be the folder's name. Any of the folder's files can
  say it, and those that do must agree. It does not rename a file imported
  on its own.

### External functions and constants

`extern` declares a function or a constant that C defines, so that Wind can
use the C library and the libraries of a platform:

```wind
extern "stdlib.h" i32 abs(i32 n)
extern "limits.h" const i32 INT_MAX

extern "string.h" {
    uint strlen(char *text)
    char *strchr(char *text, i32 c)
    i32 strcmp(char *a, char *b)
}

extern i32 putchar(i32 c)

cout << abs(-5) << " " << strlen("wind") << " " << INT_MAX << "\n"
putchar(i32('!'))
```

- The string after `extern` is the C header that declares the function or
  constant; the generated C includes it. Without one, the generated C
  declares the function itself from the Wind signature, which must then
  agree with any declaration C already has.
- `extern "header" { ... }` declares several at once: everything in the
  block shares its header. The declarations are written one to a line, or
  separated by `;`.
- `link "name"` after the header names a C library that the functions are
  in, as `link "m"` for C's maths library: `extern "math.h" link "m" { ...
  }`. A program is linked with it (`-lm`) only if it uses one of them.
- An external function has no body and an external constant no value: both
  are C's. They keep their own names in the generated C. An external
  constant can be used wherever a value can, but not where Wind needs a
  number at compile time, such as an array's length.
- Only types that C has a matching form for can cross: numbers, `bool`,
  `char`, enums and pointers. A `string` cannot; C's strings are `char *`
  (see `cstring()`). An array cannot either; pass a pointer to its elements.
- The Wind type must match C's: `int` is pointer-sized, so C's `int` is
  `i32` on a 64-bit machine. Pointers are passed as they are, whatever they
  point to, so `char *` can be given to a C function that takes `const
  char *`.
- `export extern` makes the declaration part of what a module offers, and
  `export extern "header" { ... }` exports the whole block, which is how
  `::vendor/arduino` is written. `export` goes only before the block, not
  on the lines inside it.

### The Arduino toolchain

The `arduino` target is for boards programmed with the Arduino toolchain:
any board that has an Arduino core, which `arduino-cli` and the Arduino IDE
build for. That takes in Arduino's own boards and many others, such as the
Raspberry Pi Pico. `wind run` builds a program, compiles and uploads it
with `arduino-cli`, and shows what it prints; `wind build` writes it as a
sketch for building yourself. The file says what it is for at its top:

```wind
// blink.wind
#target arduino
#board rp2040:rp2040:rpipico2w

import ::vendor/arduino

int blinks = 0
arduino.pinMode(arduino.LED_BUILTIN, arduino.OUTPUT)

void loop() {
    arduino.digitalWrite(arduino.LED_BUILTIN, arduino.HIGH)
    arduino.delay(500)
    arduino.digitalWrite(arduino.LED_BUILTIN, arduino.LOW)
    arduino.delay(500)
    blinks++
    cout << "blinks: " << blinks << "\n"
}
```

```sh
wind run blink.wind                    # compile, upload, show the output until Ctrl-C
wind run --fqbn arduino:avr:uno blink.wind          # on another board
wind run -p /dev/ttyACM1 blink.wind    # on the board at that port

wind build blink.wind                  # writes blink/blink.ino and blink/wind_program.c
wind build blink.wind -o sketches/blinker
wind check blink.wind                  # checked with the board's sizes and rules
wind emit blink.wind                   # prints the C the sketch holds
```

- **Settings.** `#target arduino` makes `run`, `build`, `check`, `emit`
  and the language server treat the file as a program for a board, and
  `#board` names the board, as `arduino-cli` spells it. They go at the very
  top of the program's file. On the command line, `-t` and `--fqbn`
  override them; without the settings a file is for the `c` target, and
  `-t arduino` makes it one for a board.
- **Running.** `wind run` needs `arduino-cli`, with the core for the board
  installed (`arduino-cli core install arduino:avr`). It looks for the
  board among the connected ones, by its name; `-p` (or `--port`) gives
  the port instead, which is needed when it is not recognized (a board in
  its bootloader, for one) or when two of them are connected. `arduino-cli
  board list` shows the names and ports. The sketch is built in a
  temporary folder, which is removed when the run ends. The output is
  shown from the moment the port can be opened after the upload; see
  "Output at the start" below.

- **Start and repeat.** The top level, or `main` in a file laid out as a C
  program, runs once when the board starts, as Arduino's `setup()`. A
  function `void loop()` is then called over and over. Without one, the
  board idles once the start has run.
- **Serial port.** `cout`, `cin` and error messages all use the board's
  serial port at 9600 baud. `cin` waits for input to arrive.
- **Errors.** An index out of range, or a `NULL` pointer that is used,
  prints its message and stops the program; the board keeps running so the
  message is sent, and starts again when it is reset.
- **The library.** `import ::vendor/arduino` brings in the pin, analog,
  time, pulse and shift functions and the board's constants (`HIGH`,
  `OUTPUT`, `LED_BUILTIN`, `A0` to `A5` and so on), all taken from the
  board's own Arduino core, and the serial port: `serialConnected()`,
  `serialAvailable()` and `waitForSerial(ms)`.
- **Output at the start.** On a board whose serial port is its own USB
  connection, such as the Pico, what is printed before a computer opens the
  port is lost, and `wind run` opens it some seconds after the upload. A
  program that should not lose its first lines starts with
  `arduino.waitForSerial(10000)`, which waits for the port to be opened, or
  ten seconds at most so that the board also starts on its own. Where the
  port goes through a separate USB chip, as on an Uno, it returns at once.
- **Sizes.** Checking assumes an AVR board (the Uno, Nano or Mega): `int`,
  `uint` and pointers are 16 bits, so `int x = 40000` is an error; use
  `i32`. A `double`, and so `float` and `f64`, is 32 bits there. `i128` and
  `u128` are not available. The sketch itself builds for any board; on a
  32-bit board `int` is 32 bits, but `sizeof` still gives the AVR sizes.
- **Not yet.** Arduino's C++-only libraries (`Wire`, `Servo`, `SPI`) and
  interrupt handlers have no declarations yet, and string literals take up
  RAM on AVR boards rather than staying in flash.

The sketch has two files: `wind_program.c`, the program as C, and a small
fixed `.ino` file that starts it from `setup()` and `loop()` and lends it the
serial port. A sketch folder must have the name of its `.ino` file, so the
folder's name is the sketch's.

### Output

`cout << a << b << ...` prints each value in turn. Any type can be printed,
and so can a whole array:
integers in decimal, floats with the fewest digits that read back as the same
value, `bool` as `true` or `false`.

### Input

`cin >> a >> b >> ...` reads one whitespace-separated value from standard
input into each variable, in turn. What is accepted depends on the variable's
type:

| Type | Input |
| --- | --- |
| Integers | a decimal number that fits the type, with an optional sign |
| Floats | a decimal or exponent number, `inf` or `nan` |
| `string` | the next word |
| `char` | the next byte that is not whitespace |
| `bool` | `true`, `false`, `1` or `0` |

Used as a value, `cin` is a `bool` telling whether the most recent read
succeeded. A read that fails (the input ran out, was not a value of the
type, or did not fit) sets the variable to its zero value:

```wind
int total = 0
for (;;) {
    cin >> int value
    if !cin { break }
    total += value
}
cout << "total: " << total << "\n"
```

A read can declare the variable it fills, by naming its type. The variable
then exists from the next statement on, like any other declaration:

```wind
cout << "Name and age: "
cin >> string name >> int age
cout << name << " is " << age << "\n"
```

- Only variables and array elements (`cin >> scores[i]`) can be read into,
  not constants or parameters.
- Pending output is flushed before reading, so a prompt printed with `cout`
  is visible even without a newline.
- Unlike in C++, a failed read does not block later reads, and the whole
  word is consumed even when only its start is a valid value.

### Current limits

- A struct field cannot be an array, and a struct or an enum cannot be read
  with `cin`; read into the fields.
- There is no `void *`, no pointer to a function, and no way to allocate
  memory while the program runs: a pointer points to a variable, an element
  or a field. `cin` reads into a variable, not through a pointer.
- A constant cannot take its value from a field of another constant.
- A message for an index that is out of range gives the line but not the
  file, which matters once a program has several files.
- A module cannot export a variable; make it a constant if its value never
  changes, or export a function that returns it.
- A constant array cannot be passed to a function, because nothing yet marks
  a parameter as read-only.
- An index must be an integer of at most 64 bits.
- Integer division by zero and converting an out-of-range float to an integer
  are not checked.
- `f128` values are printed and read with the precision of the C
  `long double`.
- A `string` read with `cin`, and a `char *` made by `cstring`, keep their
  memory until the program exits.
- `i128`, `u128`, `f16` and `f128` rely on GCC or Clang on a 64-bit target.

## Benchmark

`bench/` holds one program written in fourteen languages: the same
algorithms, each written to run fast in its language. It has
twelve kernels, each of which leans on one thing:

| Kernel | What it leans on |
| --- | --- |
| `fib` | function calls: the naive recursive Fibonacci |
| `sieve` | array writes: primes up to twenty million |
| `collatz` | integer arithmetic and branches |
| `gcd` | division inside a called function |
| `mandelbrot` | float arithmetic |
| `matmul` | nested loops over flat arrays |
| `particles` | structs of floats, updated in place |
| `sort` | recursion with swaps: quicksort of two million numbers |
| `queens` | recursion with backtracking |
| `hash` | scattered reads and writes: a hash table with open addressing |
| `bytes` | bytes: counting, hashing and searching four million letters |
| `list` | pointer chasing: a shuffled linked list of a million nodes |

```sh
python3 bench/run.py                       # everything (a few minutes); writes bench/RESULTS.md
python3 bench/run.py --skip python --skip lua
python3 bench/run.py --only wind --only c sort hash
python3 bench/run.py --seconds             # times in place of ratios
```

The script builds each program, runs every kernel as its own process, keeps
the best of five runs, and checks that every language prints the same result
for a kernel. A language whose toolchain is missing is left out.

### Results

The latest results are in [bench/RESULTS.md](bench/RESULTS.md): each
language's time relative to C over all twelve kernels, each kernel on its
own, and the toolchain versions and machine they were measured with.
`bench/run.py` writes that file whenever it runs every kernel, so running
it again on another machine replaces the numbers with that machine's.

### Reading the numbers

- **Every program is written to be fast in its own language.** They all run
  the same algorithms, with no threads, no vector instructions written by
  hand and no library sort, but each uses what its language has for speed.
  Where that changes the shape of the code, the top of the file says how.
- **Wind is C with checks.** It compiles to C, so it gets whatever the C
  compiler can do. What it adds is a range check on every array index and a
  `NULL` check on every use of a pointer. A check is one comparison that
  the processor predicts, so it shows only where indexes are worked out in
  a tight loop. With `--no-bounds-check` the difference from C is within
  the noise of the measurement.
- **Most of the others here run without those checks.** Zig, Odin and
  Swift are built with them off, and Rust and Haskell skip them in their
  hot loops with unchecked reads and writes. Go, Java, JavaScript, Lua,
  Elixir and Python keep theirs, as plain Wind does.
- **The compiler behind the language matters as much as the language.** C,
  C++ and Wind were built with GCC here; Zig, Rust, Odin and Swift use
  LLVM. GCC does better on `fib` and `matmul`, which is why those four take
  about twice as long there while matching C elsewhere. Building the C
  program with Clang gives the same doubling on `matmul`.
- **A time includes starting the program.** That is nothing for the
  compiled languages, tens of milliseconds for Java and JavaScript, whose
  kernels also start out interpreted, and about a tenth of a second for
  Elixir's virtual machine. On kernels this short it is a real part of
  their ratios.
- **Managed languages keep their numbers side by side.** Java, JavaScript,
  Lua, Haskell and Elixir hold the particles and the list's nodes as
  numbers in arrays, where objects would each be reached through a
  reference; under LuaJIT those arrays are C arrays made through its FFI.
  `list` then follows positions where C follows pointers. Python is the
  exception: there, objects turned out faster than lists of positions.
- **Haskell and Elixir are far from their usual style.** Neither changes
  data in place in ordinary code. The Haskell program uses unboxed mutable
  arrays and strict loops throughout. The Elixir program uses Erlang's
  `:atomics` for the arrays it writes to, tuples for those it only reads,
  a binary for the bytes, and whole numbers as sets of bits for `queens`.
- **The flags are the ones that helped.** Stronger settings were tried for
  C, C++, Rust and Zig (`-O3`, and building for this exact processor). They
  made one kernel faster and others slower, with no gain overall, so those
  four stay at their plain optimized builds.

### Problems

`bench/problems/` holds 22 small problems of the kind set in programming
interviews and contests, each written the same way in Wind, C, Rust, Go,
JavaScript and Python. Each folder holds one problem: dynamic programming
(`edit_distance`, `knapsack`, `coin_change`, `longest_common_subsequence`,
...), graphs and search (`dijkstra_grid`, `bfs_maze`, `topological_sort`,
`islands`, `union_find`, `sudoku`), arrays and hashing (`two_sum`,
`sliding_window_maximum`, `trapping_rain_water`, ...), and sorting, maths
and simulation (`merge_sort_inversions`, `matrix_fibonacci`, `kth_largest`,
`game_of_life`, `next_permutation`). The description is at the top of each
`main.wind`.

Unlike the kernels, every answer is checked against a known one. Each
program takes `test` or `bench` for the size of its input, makes that input
with the same small generator in every language, and prints its answer,
which must equal the folder's `expected-test.txt` or `expected-bench.txt`.
Those files come from the Python version, the reference, which is written
for clarity rather than speed.

```sh
python3 bench/problems/run.py                     # check everything at test size
python3 bench/problems/run.py --size bench        # check, time, write RESULTS.md
python3 bench/problems/run.py --only wind knapsack sudoku
python3 bench/problems/run.py --expected two_sum  # rewrite the answers from Python
```

`make test` runs the Wind version of every problem at test size, with and
without bounds checks, so the set doubles as a body of real programs that
each change to the compiler must still get right.

The latest timings are in
[bench/problems/RESULTS.md](bench/problems/RESULTS.md), which `run.py`
writes whenever it checks and times every problem at bench size with every
answer right. The programs are written plainly in every language, with no
unchecked reads in Rust and no tuning beyond sensible types, so the numbers
show what ordinary code in each language costs. Wind's bounds checks cost
most where an index is computed in a tight loop; without them Wind runs as
fast as C.

## Compiler layout

| File | Role |
| --- | --- |
| `src/lexer.c` | Source text to tokens |
| `src/parser.c` | Tokens to AST (grammar is documented at the top of the file) |
| `src/sema.c` | Name resolution, type checking and lint warnings |
| `src/codegen.c` | Checked AST to C |
| `src/targets/` | What differs between targets: `host.c` for the machine the compiler runs on, `arduino.c` for boards programmed with the Arduino toolchain |
| `src/lsp.c` | Language server |
| `src/json.c` | JSON reading and writing for the language server |
| `src/main.c` | Command-line driver |
| `src/ast.h` | AST and symbol definitions shared by the passes |
| `src/types.c` | The table of built-in types, and the platform that sizes them |
| `src/common.c` | Arena allocator and diagnostics |

The passes never abort on bad input: they record diagnostics and recover, so
one run reports every problem in a file. Code is only generated for a file
with no errors.

Adding a feature usually touches each pass once: a token in the lexer, a node
in `ast.h`, a rule in the parser, a check in `sema.c`, and a case in
`codegen.c`.

Tests live in `tests/ok` (expected stdout in `.out`), `tests/err` (expected
`wind check` output in `.err`), `tests/arduino` (sketches run against a
stand-in board on the machine running the tests) and `tests/lsp.py` (a
scripted LSP session). `tests/libs` is the libraries directory the tests
import from.
