# Wind

A small, explicit programming language in the spirit of Odin. The compiler is
written in C11 and translates Wind to C, then hands the result to the system C
compiler.

```wind
const string Greeting = "Hello"

int add(int a, int b) {
    return a + b
}

int i = 0
next:
if i < 3 {
    cout << Greeting << " " << add(i, 1) << "\n"
    i = i + 1
    goto next
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
  errors. Warnings (an unused constant, variable, function or label) do not
  fail a check or a build.
- `wind lsp` provides diagnostics as you type, hover, go-to-definition and
  completion for constants, variables, functions, parameters and labels.

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

`main` takes no parameters and returns `int` (the process exit status) or
`void`. Top-level variable initializers run, in order, before `main`.

### Basics

- A statement ends at a newline or a `;`. Semicolons are optional.
- A newline does not end a statement directly after `=`, `<<`, `>>`, `,` or a
  binary operator, or inside parentheses.
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
integers.

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
count += 2           // also -=, *=, /=, %=
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

### Operators

| Operators | Operands | Result |
| --- | --- | --- |
| `+` `-` `*` `/` | numbers | same type |
| `%` | integers | same type |
| unary `-` | numbers | same type |
| `==` `!=` | any two values of one type | `bool` |
| `<` `<=` `>` `>=` | numbers, `char` | `bool` |
| `&&` `\|\|` `!` | `bool` | `bool` |

From tightest to loosest: unary, `* / %`, `+ -`, comparisons, `&&`, `||`.
Integer arithmetic wraps at the width of its type.

### Control flow

```wind
if count > 10 {
    cout << "many\n"
} else if count > 0 {
    cout << "some\n"
} else {
    cout << "none\n"
}

again:
count = count - 1
if count > 0 { goto again }
```

- An `if` condition must be a `bool`. Parentheses around it are optional:
  `if (count > 10) {` and `if count > 10 {` are the same. Braces are
  required.
- `name:` declares a label and `goto name` jumps to it. Labels work as in C:
  a label is known throughout the function it is in (or throughout the
  top-level code of a script), so a `goto` can jump forwards or backwards,
  and into or out of blocks. `goto` is the only loop for now.

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
- Parameters cannot be assigned to.
- A function sees every other function, and the constants and variables
  declared directly at the top level above it.

### Output

`cout << a << b << ...` prints each value in turn. Any type can be printed:
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
int value
next:
cin >> value
if cin {
    total += value
    goto next
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

- Only variables can be read into, not constants or parameters.
- Pending output is flushed before reading, so a prompt printed with `cout`
  is visible even without a newline.
- Unlike in C++, a failed read does not block later reads, and the whole
  word is consumed even when only its start is a valid value.

### Current limits

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
