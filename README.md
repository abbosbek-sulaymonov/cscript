# CScript

A JavaScript-flavoured programming language, implemented from scratch in C11 as
a bytecode virtual machine.

```js
print "Hello, " + "CScript!";

print 1 + 2 * 3;        // 7 — precedence, not left-to-right
print 1 == "1";         // true  — loose equality coerces
print 1 === "1";        // false — strict equality does not
print null || "default";
print typeof null;      // "object" — yes, on purpose
```

```
$ cscript hello.cs
Hello, CScript!
```

---

## Status

**v0.1.0 — the pipeline is complete end to end.**

Source becomes tokens, tokens become a syntax tree, the tree is compiled to
bytecode, and a stack VM executes it, with a mark-sweep garbage collector
underneath. Expressions, all the operators, both equality families, JavaScript's
truthiness and coercion rules, and real error reporting all work today.

What is deliberately *not* here yet: variables, control flow, functions and
objects. Those are milestones 2 through 5, and every one of them extends a stage
that already exists rather than adding a new one — which is the whole point of
building the skeleton first. See the [roadmap](#roadmap).

---

## Table of contents

- [Build and run](#build-and-run)
- [The language](#the-language)
- [How it works](#how-it-works)
- [Development](#development)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)

---

## Build and run

No dependencies beyond a C11 compiler and `make`.

```bash
git clone https://github.com/abbosbek-sulaymonov/cscript.git
cd cscript
make
```

That produces `build/release/cscript`.

```bash
./build/release/cscript                      # REPL
./build/release/cscript examples/hello.cs    # run a file
./build/release/cscript -e 'print 6 * 7;'    # one-liner
./build/release/cscript --help
```

---

## The language

Full reference: **[docs/GRAMMAR.md](docs/GRAMMAR.md)**.

### Types

Five of JavaScript's primitives: `number`, `string`, `boolean`, `null`,
`undefined`. All numbers are IEEE 754 doubles, so `10 / 2` prints `5`, `1 / 0`
is `Infinity` and `0 / 0` is `NaN`.

### Operators

| Group | Operators |
| --- | --- |
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `<` `<=` `>` `>=` |
| Equality | `==` `!=` `===` `!==` |
| Logical | `&&` `\|\|` `!` |
| Other | `typeof`, `( )` |

### The JavaScript parts that are kept on purpose

This is a JavaScript-flavoured language, so it keeps the semantics that make
JavaScript what it is — including the awkward ones.

```js
print 1 == "1";            // true   — coerces
print 1 === "1";           // false  — does not
print 0 == false;          // true
print null == undefined;   // true
print typeof null;         // "object"
print (0/0) === (0/0);     // false  — NaN is not equal to itself
```

`&&` and `||` short-circuit and evaluate to **an operand**, not to a boolean:

```js
print null || "fallback";   // "fallback"
print "first" && "second";  // "second"
print 0 && "never";         // 0
```

`+` concatenates when either side is a string, and adds otherwise:

```js
print "n = " + 42;     // "n = 42"
print 1 + 2;           // 3
```

### One deliberate departure

`-` `*` `/` `%` require numbers and raise an error on anything else, where
JavaScript would quietly produce `NaN`:

```js
print true * 3;
// cscript: runtime error: operands of '*' must be numbers, got boolean and number
//   at <argv>:1
```

A `NaN` that propagates silently through a program is far harder to debug than
an error at the point of the mistake.

### Errors point at the problem

```
$ cscript -e 'print 1 +;'
<argv>:1: error: expected an expression
      1 | print 1 +;
        |          ^
```

The parser recovers at statement boundaries, so one bad line does not hide the
rest of the file — a script with three independent syntax errors reports all
three.

---

## How it works

Full detail: **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

```
source ──▶ Lexer ──▶ Parser ──▶ Compiler ──▶ VM ──▶ output
             │         │           │          │
          tokens      AST       bytecode    values
```

Four stages, each with one job, its own header, and a debug flag that dumps
exactly what it produced. `make trace` turns all four on at once:

```
$ ./build/trace/cscript -e 'print 1 + 2 * 3;'
== tokens ==
   1 PRINT              'print'
   | NUMBER             '1'
   | PLUS               '+'
   ...

== ast ==
Program (1 statement)
  PrintStmt
    Binary +
      Number 1
      Binary *
        Number 2
        Number 3

== <argv> ==
0000    1 OP_CONSTANT           0 '1'
0002    | OP_CONSTANT           1 '2'
0004    | OP_CONSTANT           2 '3'
0006    | OP_MULTIPLY
0007    | OP_ADD
0008    | OP_PRINT

          [ 1 ][ 2 ][ 3 ]
0006    | OP_MULTIPLY
          [ 1 ][ 6 ]
0007    | OP_ADD
          [ 7 ]
7
```

Three design decisions worth stating up front:

**A bytecode VM, not a tree walker.** Flat instruction arrays stay
cache-friendly, recursion depth is bounded by the value stack rather than the C
stack, and it is the shape every production JavaScript engine takes.

**An AST is kept anyway.** A single-pass compiler would be smaller, but the AST
is the only structure a formatter, linter or optimiser can work on. It lives
only between parsing and code generation, in an arena freed in one call.

**A garbage collector from day one.** Retrofitting GC means touching every
allocation site in the codebase. Wiring it in while there is one object type is
almost free.

---

## Development

```bash
make            # optimised build       -> build/release/cscript
make debug      # -O0 -g3 + UBSan       -> build/debug/cscript
make asan       # + AddressSanitizer    -> build/asan/cscript
make gcstress   # collects on every allocation
make trace      # debug + dumps every stage
make clean
make help
```

Each configuration builds into its own directory, so switching between them
never leaves a stale object behind.

### Tests

Golden-file suite: every `tests/cases/NAME.cs` runs and its output is compared
against `NAME.expected`. Cases named `error_*` assert the failure path.

```bash
make test              # under UBSan — undefined behaviour aborts the run
make test-gc           # collecting on every allocation
make test-asan         # under AddressSanitizer
make test FILTER=string
UPDATE=1 tests/run_tests.sh    # rewrite .expected from actual output
```

`make test-gc` is not decoration. Collecting on every single allocation means
any value the collector cannot reach from a root is freed the instant it becomes
unreachable — it caught a real bug during development, where the executing
chunk's constant pool was never being marked and string literals were collected
mid-run.

> **Note:** AddressSanitizer fails to start under some sandboxed environments,
> which is why `make test` uses the UBSan build and ASan is a separate target.

---

## Project layout

```
cscript/
├── include/cscript/     one public header per module
├── src/
│   ├── lexer.c          source text  -> tokens
│   ├── parser.c         tokens       -> AST   (precedence climbing, recovery)
│   ├── ast.c            node constructors and the arena
│   ├── compiler.c       AST          -> bytecode
│   ├── vm.c             the interpreter loop
│   ├── memory.c         allocator and mark-sweep collector
│   ├── object.c         heap objects, string interning
│   ├── table.c          open-addressing hash table
│   ├── value.c          value operations, coercion, formatting
│   ├── chunk.c          bytecode buffer and constant pool
│   ├── debug.c          disassembler and AST printer
│   ├── diagnostic.c     error reporting with source spans
│   └── main.c           CLI, REPL, file runner
├── tests/
│   ├── run_tests.sh     golden-file runner
│   └── cases/           *.cs paired with *.expected
├── examples/            runnable sample programs
└── docs/
    ├── ARCHITECTURE.md  how the pipeline and the GC work
    └── GRAMMAR.md       full grammar and semantics
```

---

## Roadmap

| Milestone | Adds | Mostly touches |
| --- | --- | --- |
| **1 ✅** | **Lexer, parser, compiler, VM, GC, REPL, expressions** | — |
| 2 | `let` / `const`, scopes, assignment | parser, compiler, new opcodes |
| 3 | `if` / `else` / `while` / `for`, blocks | parser, compiler (jumps already exist) |
| 4 | Functions, call frames, closures | VM gains a frame stack |
| 5 | Objects, arrays, property access | new `Obj` types |
| 6 | `console.log` and native functions | replaces the temporary `print` statement |
| 7 | NaN-boxing, computed-goto dispatch | `value.h` and the VM loop |

`print` is scaffolding: milestone 1 has no functions to call, so it is a
statement for now and becomes `console.log` in milestone 6.

---

## Author

**Abbosbek Sulaymonov** — [@abbosbek-sulaymonov](https://github.com/abbosbek-sulaymonov) · [abbosbek.uz](https://abbosbek.uz)

## License

MIT — see [LICENSE](LICENSE).
