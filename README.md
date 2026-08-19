# CScript

A programming language with **JavaScript's syntax and none of its footguns**,
implemented from scratch in C11 as a bytecode virtual machine.

```js
// hello.cx — this is also a valid JavaScript file
for (let i = 1; i <= 15; i++) {
  if (i % 15 === 0) {
    console.log("FizzBuzz");
  } else if (i % 3 === 0) {
    console.log("Fizz");
  } else if (i % 5 === 0) {
    console.log("Buzz");
  } else {
    console.log(i);
  }
}
```

```
$ cscript examples/fizzbuzz.cx
1
2
Fizz
...
```

---

## The idea

**Every CScript program is a valid JavaScript program.** Same keywords, same
operators, same precedence, same `console.log`. `make test-node` runs the
examples under Node and diffs the output, so that claim stays honest rather than
aspirational.

**The semantics are not JavaScript's.** CScript removes the traps that make
JavaScript hard to get right — and every removal is a **loud error**, never a
silent change of meaning. A program that would behave differently here refuses
to compile or fails at runtime, so nothing you write can quietly mean one thing
in CScript and another in Node.

```js
1 == "1"      // error: '==' coerces its operands; use '==='
var x = 1;    // error: 'var' is function-scoped and hoisted; use 'let' or 'const'
true * 3      // runtime error: operands of '*' must be numbers
console.log(typo);  // runtime error: 'typo' is not defined
answer = 43;  // error: 'answer' is declared const and cannot be reassigned
typeof null   // "null", not "object"
```

The full list, with the reasoning for each, is in
[docs/GRAMMAR.md](docs/GRAMMAR.md#fixes-to-javascript).

---

## Status

**v0.2.0.** Variables, block scoping, control flow, function calls, property
access and the `console` / `Math` built-ins all work, on top of the milestone-1
pipeline: lexer → parser → bytecode compiler → stack VM, with a mark-sweep
collector underneath.

Not here yet: user-defined functions, object literals and arrays. Each produces
an error naming the milestone it lands in. See the [roadmap](#roadmap).

---

## Build and run

No dependencies beyond a C11 compiler and `make`.

```bash
git clone https://github.com/abbosbek-sulaymonov/cscript.git
cd cscript
make
```

```bash
./build/release/cscript                       # REPL
./build/release/cscript examples/hello.cx     # run a file
./build/release/cscript -e 'console.log(6*7);'
./build/release/cscript --help
```

> Source files use **`.cx`**. `.cs` belongs to C#.

---

## The language

Full reference: **[docs/GRAMMAR.md](docs/GRAMMAR.md)**.

```js
// Variables — let and const only, both block-scoped.
const limit = 10;
let total = 0;

for (let i = 1; i <= limit; i++) {
  total += i * i;
}
console.log("sum of squares:", total);   // 385

// Blocks scope, and inner names shadow outer ones.
let name = "outer";
{
  let name = "inner";
  console.log(name);   // inner
}
console.log(name);     // outer

// && and || return an operand, not a boolean, and short-circuit.
console.log(null || "fallback");   // fallback
console.log(0 && "never");         // 0

// Conversions are explicit.
console.log(Number("42") + 1);     // 43
console.log(String(42) + "!");     // 42!
```

| Group | Operators |
| --- | --- |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` |
| Arithmetic | `+` `-` `*` `/` `%` `++` `--` |
| Comparison | `<` `<=` `>` `>=` |
| Equality | `===` `!==` |
| Logical | `&&` `\|\|` `!` |
| Other | `typeof` `.` `( )` |

Built-ins: `console.log` / `.error` / `.warn`, `Math.floor` / `.abs` / `.max` /
`.min` / `.PI` / `.E`, and `Number` / `String` / `Boolean`. All are constants.

### Errors point at the problem

```
$ cscript -e 'console.log(1 +);'
<argv>:1: error: expected an expression
      1 | console.log(1 +);
        |                ^
```

The parser recovers at statement boundaries, so one bad line does not hide the
rest of the file.

---

## How it works

Full detail: **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

```
source ──▶ Lexer ──▶ Parser ──▶ Compiler ──▶ VM ──▶ output
             │         │           │          │
          tokens      AST       bytecode    values
```

Four stages, each with one job, its own header, and a debug flag that dumps
exactly what it produced. `make trace` turns all four on at once.

Design decisions worth stating up front:

**A bytecode VM, not a tree walker.** Flat instruction arrays stay
cache-friendly, recursion depth is bounded by the value stack rather than the C
stack, and it is the shape every production JavaScript engine takes.

**An AST is kept anyway.** A single-pass compiler would be smaller, but the AST
is the only structure a formatter, linter or optimiser can work on. It lives
only between parsing and code generation, in an arena freed in one call.

**Scopes are resolved at compile time.** A local variable compiles to a stack
slot index, so reading a loop counter is an array index — not the hash lookup a
naive interpreter would do on every iteration. Globals still hash, which is a
real reason to prefer `let` inside a loop.

**A garbage collector from day one.** Retrofitting GC means touching every
allocation site. Wiring it in while there was one object type was nearly free —
and it paid off immediately when milestone 2 added two more.

---

## Development

```bash
make            # optimised build       -> build/release/cscript
make debug      # -O0 -g3 + UBSan       -> build/debug/cscript
make asan       # + AddressSanitizer
make gcstress   # collects on every allocation
make trace      # debug + dumps every stage
make clean
make help
```

Each configuration builds into its own directory, so switching between them
never leaves a stale object behind.

### Tests

```bash
make test        # golden-file suite under UBSan — UB aborts the run
make test-gc     # same suite, collecting on every allocation
make test-node   # examples must match Node.js output
make test-all    # all three
make test FILTER=scoping
UPDATE=1 tests/run_tests.sh    # rewrite .expected from actual output
```

Every `tests/cases/NAME.cx` runs and its output is compared against
`NAME.expected`; cases named `error_*` assert the failure path.

**`make test-gc` is not decoration.** Collecting on every single allocation
means any value the collector cannot reach from a root is freed the instant it
becomes unreachable. It caught a real bug in milestone 1 — the executing chunk's
constant pool was never marked, so string literals were collected mid-run.

**`make test-node` is what keeps the headline claim true.** It runs each example
under Node and requires byte-identical output, except for the files listed as
deliberately divergent — which it requires to actually differ, so a fix that
silently stops working also fails the build.

> AddressSanitizer fails to start under some sandboxed environments, which is
> why `make test` uses the UBSan build and ASan is a separate target.

---

## Project layout

```
cscript/
├── include/cscript/     one public header per module
├── src/
│   ├── lexer.c          source text  -> tokens
│   ├── parser.c         tokens       -> AST   (precedence climbing, recovery)
│   ├── ast.c            node constructors and the arena
│   ├── compiler.c       AST          -> bytecode (scope resolution lives here)
│   ├── vm.c             the interpreter loop
│   ├── native.c         console, Math, and the conversion functions
│   ├── memory.c         allocator and mark-sweep collector
│   ├── object.c         heap object types, string interning
│   ├── table.c          open-addressing hash table
│   ├── value.c          value operations, coercion, formatting
│   ├── chunk.c          bytecode buffer and constant pool
│   ├── debug.c          disassembler and AST printer
│   ├── diagnostic.c     error reporting with source spans
│   └── main.c           CLI, REPL, file runner
├── tests/
│   ├── run_tests.sh     golden-file runner
│   ├── node_parity.sh   examples vs. Node.js
│   └── cases/           *.cx paired with *.expected
├── examples/            runnable sample programs
└── docs/
    ├── ARCHITECTURE.md  how the pipeline and the GC work
    └── GRAMMAR.md       full grammar, semantics, and every fix to JavaScript
```

---

## Roadmap

| Milestone | Adds | Mostly touches |
| --- | --- | --- |
| **1 ✅** | Lexer, parser, compiler, VM, GC, REPL, expressions | — |
| **2 ✅** | `let`/`const`, scopes, control flow, calls, `console.log` | — |
| 3 | User functions, `return`, closures | VM gains a frame stack |
| 4 | Object literals, arrays, indexing | `ObjObject` already exists |
| 5 | `switch`, `break`/`continue`, `for...of` | parser and compiler |
| 6 | Template literals, ternary, destructuring | parser and compiler |
| 7 | NaN-boxing, computed-goto dispatch | `value.h` and the VM loop |

---

## Author

**Abbosbek Sulaymonov** — [@abbosbek-sulaymonov](https://github.com/abbosbek-sulaymonov) · [abbosbek.uz](https://abbosbek.uz)

## License

MIT — see [LICENSE](LICENSE).
