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
build/wind check examples/hello.wind        # report errors and warnings only
build/wind emit-c examples/hello.wind       # print the generated C
build/wind lsp                              # language server on stdio

make install    # installs to ~/.local/bin/wind (override with PREFIX=)
```

Requires GCC or Clang on a POSIX system. `$CC` selects the C compiler used
both for building `wind` and by `wind` itself (default `cc`).

`make test` also needs `python3`, which drives the language server test.

## Tooling

The linter and language server are built into the `wind` binary and share the
compiler's own lexer, parser and checker, so an editor reports exactly what a
build would.

- `wind check <file>...` prints every diagnostic as
  `file:line:col: severity: message` and exits with status 1 if there are
  errors. Warnings do not fail a check or a build. They cover an unused
  constant, variable, function, label or import, and unreachable code.
  Problems in an imported file are listed under that file's own name.
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
  binary operator, inside parentheses, or before a line that starts with the
  `?` or `:` of a conditional.
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
- Elements are indexed from zero. A negative index counts back from the
  end, as in Python: `scores[-1]` is the last element and `scores[-2]` the
  one before it.
- Every index is checked: one that is out of range, in either direction,
  stops the program with a message naming the line, the index and the
  length. A literal index that is out of range is a compile-time error.
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

An array is not a value in itself. Its name can be indexed, measured,
printed, and passed to an array parameter, but one array cannot be assigned to another,
compared with `==`, or returned from a function. Arrays have one dimension,
and their elements cannot themselves be arrays.

### Operators

| Operators | Operands | Result |
| --- | --- | --- |
| `+` `-` `*` `/` | numbers | same type |
| `%` | integers | same type |
| unary `-` | numbers | same type |
| `==` `!=` | any two values of one type | `bool` |
| `<` `<=` `>` `>=` | numbers, `char` | `bool` |
| `&&` `\|\|` `!` | `bool` | `bool` |
| `&` `\|` `^` (and, or, xor) | integers | same type |
| unary `~` (not) | integers | same type |
| `<<` `>>` (shifts) | an integer, shifted by any integer | type of the value shifted |

| `c ? a : b` (conditional) | a `bool`, then two values of one type | that type |

From tightest to loosest: unary and casts, `* / %`, `+ -`, `<< >>`, `&`, `^`,
`|`, comparisons, `&&`, `||`, `?:`. Integer arithmetic wraps at the width of
its type.

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
  it, but a type must be declared above another type that uses it.
- `Name(value)` and `(Name)value` convert to a declared type, as they do for
  built-in ones.
- `export` works on `struct`, `enum` and `typedef`. An importing file writes
  `module.Point`, and an exported enum's names as `module.RED`.

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
import "shapes.wind"
import "lib/my-text.wind" as text

cout << shapes.area(3, 4) << " " << shapes.SIDES << "\n"
```

- A path is relative to the file that contains the import.
- The module's name is the file's name without `.wind`; `as name` gives it
  another. A file whose name is not a valid name, such as `my-text.wind`,
  needs `as`.
- What a module exports is used as `module.name`. Names never leak into the
  importing file, so two modules can export the same name.
- Functions, constants and types can be exported. Everything else in a file
  is private to it, including its top-level variables, which its functions can
  use and which are initialized before the program starts.
- A file is loaded once, however many files import it, so its variables
  exist once. Imports cannot form a cycle.
- Imports and `export` go at the top level. An import can be used anywhere
  in the file, including above it.
- An imported file is not run as a script: it holds declarations only. It may
  have a `main`, which is ignored when the file is imported, so a module can
  also be run on its own.
- The program is compiled as a whole: `wind build main.wind` follows the
  imports and produces one executable.

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
- A constant cannot take its value from a field of another constant.
- A message for an index that is out of range gives the line but not the
  file, which matters once a program has several files.
- A module cannot export a variable; export a function that returns it.
- A constant array cannot be passed to a function, because nothing yet marks
  a parameter as read-only.
- An index must be an integer of at most 64 bits.
- Integer division by zero and converting an out-of-range float to an integer
  are not checked.
- `f128` values are printed and read with the precision of the C
  `long double`.
- A `string` read with `cin` keeps its memory until the program exits.
- `i128`, `u128`, `f16` and `f128` rely on GCC or Clang on a 64-bit target.

## Compiler layout

| File | Role |
| --- | --- |
| `src/lexer.c` | Source text to tokens |
| `src/parser.c` | Tokens to AST (grammar is documented at the top of the file) |
| `src/sema.c` | Name resolution, type checking and lint warnings |
| `src/codegen.c` | Checked AST to C |
| `src/lsp.c` | Language server |
| `src/json.c` | JSON reading and writing for the language server |
| `src/main.c` | Command-line driver |
| `src/ast.h` | AST and symbol definitions shared by the passes |
| `src/types.c` | The table of built-in types |
| `src/common.c` | Arena allocator and diagnostics |

The passes never abort on bad input: they record diagnostics and recover, so
one run reports every problem in a file. Code is only generated for a file
with no errors.

Adding a feature usually touches each pass once: a token in the lexer, a node
in `ast.h`, a rule in the parser, a check in `sema.c`, and a case in
`codegen.c`.

Tests live in `tests/ok` (expected stdout in `.out`), `tests/err` (expected
`wind check` output in `.err`) and `tests/lsp.py` (a scripted LSP session).
