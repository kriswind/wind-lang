# Wind

A small, explicit programming language in the spirit of Odin. The compiler is
written in C11 and translates Wind to C, then hands the result to the system C
compiler.

```wind
const string s = "A string of characters"
cout << "My String: " << s << "\n"
```

## Build and use

```sh
git clone https://github.com/kriswind/wind-lang.git
cd wind-lang

make            # produces build/wind
make test

build/wind run examples/hello.wind          # compile and run
build/wind build examples/hello.wind -o hi  # compile to an executable
build/wind check examples/hello.wind        # report errors and warnings only
build/wind emit-c examples/hello.wind       # print the generated C
build/wind lsp                              # language server on stdio

make install    # installs to ~/.local/bin/wind (override with PREFIX=)
```

Requires a C11 compiler on a POSIX system. `$CC` selects the C compiler used
both for building `wind` and by `wind` itself (default `cc`).

`make test` also needs `python3`, which drives the language server test.

## Tooling

The linter and language server are built into the `wind` binary and share the
compiler's own lexer, parser and checker, so an editor reports exactly what a
build would.

- `wind check <file>...` prints every diagnostic as
  `file:line:col: severity: message` and exits with status 1 if there are
  errors. Warnings (such as an unused constant) do not fail a check or a build.
- `wind lsp` provides diagnostics as you type, hover, go-to-definition and
  completion.

Setup for Neovim (including Mason), Sublime Text and Helix is in
[editors/README.md](editors/README.md).

## The language

A program is a sequence of top-level statements, executed in order.

### Statements

| Statement | Form |
| --- | --- |
| Constant declaration | `const string Name = <expr>` |
| Console output | `cout << <expr> << <expr> ...` |

An expression is a string literal or the name of a constant.

- A statement ends at a newline or a `;`. Semicolons are optional.
- A newline directly after `=` or `<<` continues the statement.
- `//` starts a comment that runs to the end of the line.
- Names are `[A-Za-z_][A-Za-z0-9_]*`; `const`, `string` and `cout` are reserved.

### Strings

A `string` is a contiguous array of bytes with an explicit length (a pointer
and a length, as in Odin). It is not NUL-terminated, so it may contain any
byte, including `\0`.

Literal escapes: `\n` `\t` `\r` `\0` `\\` `\"`. A literal cannot span lines.

### Constants

Constants are compile-time values. A constant must be defined before it is
used, cannot be redefined, and cannot be assigned to.

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
| `src/ast.h` | AST and type definitions shared by the passes |
| `src/common.c` | Arena allocator and diagnostics |

The passes never abort on bad input: they record diagnostics and recover, so
one run reports every problem in a file. Code is only generated for a file
with no errors.

Adding a feature usually touches each pass once: a token in the lexer, a node
in `ast.h`, a rule in the parser, a check in `sema.c`, and a case in
`codegen.c`.

Tests live in `tests/ok` (expected stdout in `.out`), `tests/err` (expected
`wind check` output in `.err`) and `tests/lsp.py` (a scripted LSP session).
