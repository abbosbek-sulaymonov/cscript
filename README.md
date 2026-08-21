# CScript

A **gradually typed** language with **TypeScript's syntax and none of
JavaScript's footguns**, implemented from scratch in C11 as a bytecode virtual
machine.

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

**Every CScript program is a valid TypeScript program.** Same keywords, same
operators, same precedence, same `console.log`, same `: number` annotations.
`make test-node` hands each example to Node with `--experimental-strip-types`
and diffs the output, so that claim stays mechanically enforced rather than
aspirational.

**Types are optional, and inferred when you leave them off.**

```ts
const name: string = "cscript";   // annotated
let year = 2026;                  // inferred as number — just as checked
let loose: any = 1;               // opted out

let total: number = "text";   // error: cannot assign string to 'total'
true * 3;                     // error: operand of '*' must be a number
```

Those are **compile** errors now, not runtime ones — and the last needs no
annotation at all.

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

**v0.19.0.** The language is feature-complete for everyday code: objects and
arrays, functions and closures, classes and inheritance, modules, promises and
`async`/`await`, gradual typing with inference, control flow, `switch`,
template literals and the conditional operator. The pipeline is lexer → parser → type checker → bytecode
compiler → stack VM, with a mark-sweep collector underneath.

```ts
class Person {
  constructor(name: string, born: number) {
    this.name = name;
    this.born = born;
  }

  label(): string {
    return `${this.name} (${this.born})`;
  }
}

const people = [new Person("Ada", 1815), new Person("Alan", 1912)];
const names = people.map(p => p.name).filter(n => n.length > 3).sort();

try {
  console.log(JSON.stringify({ names: names, first: people[0].label() }));
} catch (e) {
  console.log(e.message);
}
```

Functions, closures, objects, arrays, classes, a standard library, gradual
typing, exceptions, destructuring and spread. What remains is modules and
async — see the [roadmap](#roadmap).

---

## Documentation

| | |
| --- | --- |
| [docs/GRAMMAR.md](docs/GRAMMAR.md) | The syntax as EBNF, and what each construct means |
| [docs/JAVASCRIPT.md](docs/JAVASCRIPT.md) | Feature by feature against JavaScript: same, deliberately different, missing |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works, and the measurements behind each decision |
| [docs/README.md](docs/README.md) | Where to look for what |

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

// Classes, with fields, inheritance and `this`.
class Shape {
  sides = 0;
  constructor(name) { this.name = name; }
  describe() { return `${this.name}: ${this.sides} sides`; }
}

class Square extends Shape {
  sides = 4;
  constructor() { super("square"); }
}

console.log(new Square().describe());        // square: 4 sides
console.log(new Square() instanceof Shape);  // true
```

| Group | Operators |
| --- | --- |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `**=` |
| Arithmetic | `+` `-` `*` `/` `%` `**` `++` `--` |
| Comparison | `<` `<=` `>` `>=` |
| Equality | `===` `!==` |
| Logical | `&&` `\|\|` `!` |
| Other | `typeof` `instanceof` `?:` `...` `.` `[ ]` `( )` `=>` |

Built-ins: `console`, `Math`, `Object`, `Array`, `Number`, `JSON`, plus
`String` / `Boolean` / `Error` / `parseInt` / `parseFloat` / `isNaN` /
`isFinite` / `NaN` / `Infinity`. Arrays carry 21 methods and strings 21 more.
All of it is read-only — `Math.PI = 3` is an error, not a surprise later.

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
naive interpreter would do on every iteration. Globals still hash: `bench/`
measures the same program both ways and locals come out about 30% faster.

**A garbage collector from day one.** Retrofitting GC means touching every
allocation site. Wiring it in while there was one object type was nearly free —
and it paid off immediately when milestone 2 added two more.

---

## Performance

Measured on an Apple M3 Pro, best of seven, via `bench/run.sh`.

```
startup: cscript 2 ms, node 37 ms — subtracted from the compute column

benchmark          cscript       node     cscript       node    ratio
                   (total)    (total)   (compute)  (compute)
---------------------------------------------------------------------
arrays               69 ms      45 ms       67 ms       8 ms     8.4x
locals              148 ms      43 ms      146 ms       6 ms    24.3x
loop_empty          160 ms      42 ms      158 ms       5 ms    31.6x
globals             195 ms      43 ms      193 ms       6 ms    32.2x
branches            210 ms      44 ms      208 ms       7 ms    29.7x
strings             216 ms      39 ms      214 ms       2 ms   107.0x
classes             231 ms      50 ms      229 ms      13 ms    17.6x
loop_arith          228 ms      46 ms      226 ms       9 ms    25.1x
properties          307 ms      46 ms      305 ms       9 ms    33.9x

native reference (loop_arith, total):  Go 8 ms   ·   C -O2 8 ms
```

**CScript is 8–34× slower than Node on compute, and roughly 35× slower than Go
or C.** The `strings` figure of 107× is not a real ratio: V8 can see that loop
produces nothing and removes it.

That is a much worse number than this table used to report, and the correction
is worth more than the result. The harness called `python3` twice per
repetition to read the clock, which put that interpreter's own ~19 ms startup
*inside every measurement*. A constant offset does not merely add noise — it
flatters whichever program is slower, because it is a smaller share of a bigger
number. Node's 37 ms of process startup was doing the same thing from the other
side. Together they turned a 25× gap into a reported 4×.

Both columns are kept because both are true. A CLI that runs for 40 ms really
does start 18× faster than Node; an interpreter loop really is 25× slower.
Quoting only the first would be the mistake that was already being made.

Three optimisations account for the 32% improvement, and the measurements are
worth more than the summary:

- **An integer fast path for `%`** was the largest single win. `OP_MODULO`
  called `fmod()` unconditionally — ten million `fmod` calls cost 182 ms
  against 8 ms for integer remainder.
- **`OP_INC_LOCAL`** collapses `i++` in statement position from seven
  instructions to one, since the old value nobody reads no longer has to be
  produced. The canonical loop body went from 16 instructions to 11.
- **Computed-goto dispatch turned out to be worth nothing.** It wins on four of
  six benchmarks and loses 19% on the branch-heavy one, netting a 0.5%
  difference. Expected: 15–25%.
- **NaN-boxing turned out to be worth nothing for speed either** — but it
  halves memory, taking a million-element array from 18.9 MB to 11.3 MB. It is
  filed as a memory optimisation for that reason.
- **Removing an instruction is worth about a third of an instruction.** The
  most useful number here, and measured rather than assumed. Fusing the three
  most frequent opcode pairs cut executed instructions by 8.5% on `classes`,
  17% on `properties` and 19% on `locals` — and cut *time* by 0.5%, 4.6% and
  9.8%. Instruction count converts to wall clock at roughly 0.25–0.5×, and on
  the object-heavy benchmark barely at all.

  The reason is that CScript's instructions already do real work each: an
  inline-cache probe, a shape compare, a NaN-boxed move. Dispatch is a smaller
  share of the total here than in the naive interpreters the textbook figures
  come from. It also prices a register VM, which is the same trade at a larger
  scale — cutting the 35–45% of instructions that exist only to move values on
  and off the stack should be worth 10–20%, not the 20–40% the technique is
  usually credited with.
- **A superinstruction has to keep its own handler.** The first attempt had the
  fused opcodes jump into the body of the one they specialise, to avoid
  duplicating forty lines. That made `classes` *slower* despite executing 8.5%
  fewer instructions: the dispatch table's indirect branch is predicted per
  opcode, and sharing one body throws that away.
- **Hidden classes and inline caches gave 11–13%**, not the 2× the technique is
  usually credited with. The reason is worth stating: CScript interns every
  property name and precomputes its hash, so the lookup being replaced was
  already one masked index and one pointer compare. The cache removes real
  work, but there was less of it there than the technique assumes. Property
  reads are also only about a fifth of the instructions in a property-heavy
  loop, which bounds what any change to them can do.

- **Superinstructions worked**, and were the first optimisation picked from a
  profile rather than from intuition. Fusing the three hottest opcode pairs cut
  the canonical loop body from 16 instructions to 7, and total instructions
  executed by 31%.

Three optimisations aimed at the cost of *executing* an instruction and found it
already nearly free. The fourth attacked the *number* of instructions and
landed. The difference was not sophistication — it was measuring first. The
reasoning is in
[ARCHITECTURE.md](docs/ARCHITECTURE.md#what-the-measurements-add-up-to).

Closing the gap with Go needs a JIT, not another tweak — the reasoning is in
[ARCHITECTURE.md](docs/ARCHITECTURE.md#the-interpreter-ceiling). There is one
now, and on the code it compiles it does close most of it:

| | interpreted | compiled | speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/jit/jit_calls.cx` — 3M calls | 136 ms | 112 ms | 1.2× | 8 ms |
| `bench/jit/jit_loop.cx` — one call, 20M iterations | 492 ms | **55 ms** | **8.9×** | 35 ms |
| `bench/locals.cx` — an ordinary script, no annotations | 149 ms | **18 ms** | **8.3×** | 7 ms |
| `bench/loop_empty.cx` — loop overhead alone, over a global | 170 ms | **20 ms** | **8.6×** | 7 ms |
| `bench/globals.cx` — reads and writes of module bindings | 211 ms | **41 ms** | **5.2×** | 7 ms |

The last three have no type annotations in them at all. Their types come from
the checker's inference of `let a = 0`, and their loops reach the compiler
because everything the compiler cannot express — the `console.log` at the end —
is handed back to the interpreter rather than refusing the whole function.

`%` calls `fmod`, because arm64 has no floating-point remainder and the inline
form stops being exact past 2^53. A function that calls anything allocates only
from the callee-saved registers, so nothing has to be spilled around the call —
but ten million libm calls still cost what they cost: `bench/loop_arith.cx`
gains 1.6× and `bench/branches.cx` 1.4×, against 5–9× for everything else.

Both columns are the same binary; only the tiering threshold differs. The
backend compiles functions whose arithmetic the type checker has already
proved, so it emits no type tests and has nothing to deoptimise to — which is
something a JavaScript engine cannot do, because JavaScript promises nothing
about a value until it sees one.

It took four optimisations that measured at nothing and one diagnosis that the
loop benchmark had never entered the compiled code at all. That story, which is
the more useful half, is in
[ARCHITECTURE.md](docs/ARCHITECTURE.md#stage-4-the-loop-and-what-was-actually-wrong-with-it).

```bash
bench/run.sh              # everything
bench/run.sh loop         # only matching names
REPS=7 bench/run.sh       # more repetitions
BIN=build/switch/cscript bench/run.sh    # compare dispatch strategies
make bench-jit            # what the JIT is worth, on what it can compile
```

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
make test         # golden-file suite under UBSan — UB aborts the run
make test-gc      # same suite, collecting on every allocation
make test-switch  # same suite, forcing the portable switch dispatch
make test-tagged  # same suite, forcing the 16-byte tagged-union Value
make test-node    # examples must match Node.js output
make test-all     # all five
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
| **3 ✅** | Gradual typing: annotations, inference, checking | — |
| **4 ✅** | User functions, `return`, closures, typed signatures | — |
| **5 ✅** | Object literals, arrays, indexing, `.length` | — |
| **6 ✅** | `switch`, `break`/`continue`, template literals, ternary | — |
| **7 ✅** | NaN-boxed values — halves memory, measured | — |
| **8 ✅** | Superinstructions chosen from an opcode profile | — |
| **9 ✅** | Method dispatch, array and string methods | — |
| **10 ✅** | `Object`, `Array`, `Number`, `JSON`, full `Math` | — |
| **11 ✅** | `**`, arrow functions, `for...of` | — |
| **12 ✅** | `try` / `catch` / `finally` / `throw` | — |
| **13 ✅** | Destructuring and spread | — |
| **14 ✅** | Hidden classes and inline caches for properties and globals | — |
| **15 ✅** | `class`, `new`, `this`, `extends`, `super`, `instanceof` | — |
| **16 ✅** | Modules — `import`, `export`, per-file scope | — |
| **17 ✅** | Promises, timers, the event loop, `async`/`await` | — |
| **18 ✅** | The rest of the syntax: patterns, accessors, `do`/`for...in` | — |
| **19 ✅** | Split the three largest files; documented against JavaScript | — |
| **20 ✅** | Tiering: what gets hot, and how much of it is already typed | — |
| **21 ✅** | `Map` and `Set`, patterns as `for...of` bindings | — |
| **22 ✅** | Regular expressions: literals, `test`/`exec`, string methods | — |
| **23 ✅** | A typed IR, verified by running it instead of the interpreter | — |
| **24 ✅** | An arm64 backend — correct, verified, and not yet faster | — |
| **25 ✅** | Register allocation: −5% on calls, still level on loops | — |
| **26 ✅** | On-stack replacement — loops **8.9× faster**, 1.6× of Node | — |
| **27 ✅** | `?.`, `??`, `&&=`/`\|\|=`/`??=`, object-literal methods | — |
| **28 ✅** | `in`, `delete`, object spread and rest, labelled statements | — |
| **29 ✅** | `#private` members, `static { }` blocks | — |
| **30 ✅** | Default exports, re-exports, `export *` | — |
| **31 ✅** | Generators — `function*`, `yield`, `yield*`, pull-driven `for...of` | — |
| **32 ✅** | Top-level `await`, `for await` over sync iterables | — |
| **33 ✅** | Regex backreferences and lookahead, function replacers, `exec().index` | — |
| **34 ✅** | `Promise.allSettled` / `.any`, `AggregateError`, `setInterval` | — |
| **35 ✅** | Async generators — `async function*`, `for await` over one | — |
| **36 ✅** | Side exits — the compiler takes the loop and leaves the rest | — |
| **37 ✅** | Globals in a compiled loop — `loop_empty` **8.6×**, `globals` **5.2×** | — |
| **38 ✅** | Calling out — `%` compiles, with nothing spilled around the call | — |
| **39 ✅** | Default parameters, class expressions, `at`/`flat`/`flatMap`, number formatting | — |
| **40 ✅** | Rest parameters, `call`/`apply`/`bind`, tagged templates | — |
| **41 ✅** | Computed class members, `new` on any expression, object-literal accessors | — |
| **42 ✅** | `Date` — the last large missing built-in | — |
| **43 ✅** | `WeakMap` and `WeakSet`, with ephemeron marking in the collector | — |
| **44 ✅** | `Symbol`, symbol-keyed properties, and `Symbol.iterator` | — |
| **45 ✅** | `BigInt` — arbitrary precision, and a checked `number` boundary | — |
| **46 ✅** | Prototypes — `Object.create`, `__proto__`, and a chain reads walk | — |
| **47 ✅** | Constructor functions — `new F()`, `F.prototype`, and call-site `this` | — |
| **48 ✅** | Inherited and static accessors, `constructor`, a `toString` that is called | — |
| next | Calling a CScript function from compiled code, which needs frames and safepoints | — |
| next | Inlining, so a small function is worth compiling | — |
| next | Guards and deoptimisation, for code the types do not prove | — |

---

## Author

**Abbosbek Sulaymonov** — [@abbosbek-sulaymonov](https://github.com/abbosbek-sulaymonov) · [abbosbek.uz](https://abbosbek.uz)

## License

MIT — see [LICENSE](LICENSE).
