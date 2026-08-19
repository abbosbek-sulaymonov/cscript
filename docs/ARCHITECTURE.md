# Architecture

CScript runs a script in four stages. Each one has a single job, its own header
in `include/cscript/`, and a debug flag that dumps what it produced.

```
  source text
      │
      ▼
  ┌─────────┐   Token stream        src/lexer.c        CS_DEBUG_PRINT_TOKENS
  │  Lexer  │   borrows the source, copies nothing
  └─────────┘
      │
      ▼
  ┌─────────┐   AST                 src/parser.c       CS_DEBUG_PRINT_AST
  │ Parser  │   arena-allocated, freed in one call
  └─────────┘
      │
      ▼
  ┌─────────┐   Chunk               src/compiler.c     CS_DEBUG_PRINT_CODE
  │Compiler │   bytecode + constant pool + line table
  └─────────┘
      │
      ▼
  ┌─────────┐   Values              src/vm.c           CS_DEBUG_TRACE_EXECUTION
  │   VM    │   stack machine
  └─────────┘
```

## Why a bytecode VM and not a tree walker

A tree-walking interpreter is less code, but every operation costs a virtual
dispatch and a pointer chase through nodes scattered across the heap, and deep
expressions consume the C stack. A bytecode VM keeps instructions in a flat
array, so the interpreter loop stays cache-friendly and recursion depth is
bounded by the value stack rather than the C stack. It is also the shape every
production JavaScript engine takes, which keeps later work — constant folding, a
jump-threaded dispatch loop, an inline cache — on the table.

## Why keep an AST at all

A single-pass compiler that emits bytecode straight from the parser is smaller
still. The AST is kept because it is the only structure a formatter, a linter,
a static analyser or an optimisation pass can work on. Adding one back later
means rewriting the parser; keeping it now costs one extra traversal.

The AST is deliberately *not* long-lived. It exists between parsing and code
generation, and `csInterpret` frees the whole arena the moment `csCompile`
returns.

## Memory

### Two allocators, on purpose

**The AST arena** (`src/ast.c`) bump-allocates from 64 KB blocks and frees
everything at once. AST nodes are small, numerous, and all die at the same
instant, so per-node bookkeeping would be pure overhead.

**The garbage-collected heap** (`src/memory.c`) owns everything that outlives
compilation — today that is strings, and soon objects, arrays and closures.

Every GC-heap byte passes through `csReallocate`, which is what lets the
collector track `bytesAllocated` and decide when to run.

### The collector

Mark-sweep, tri-colour, non-moving:

1. **Mark roots** — the value stack, the globals table, the const-globals
   table, the temporary-root stack, the constant pool of the chunk being
   executed, and the constant pool of the chunk being compiled.
2. **Trace** — drain a worklist, blackening each object's references.
3. **Sweep weak references** — drop intern-pool entries whose string died.
4. **Sweep** — walk the intrusive object list and free anything unmarked.

`vm.nextGC` doubles after each cycle, so collection cost stays proportional to
live data rather than to allocation rate.

### The two root sets that are easy to miss

Both of these caused real bugs during development, caught by `make test-gc`:

- **The chunk being executed.** `OP_CONSTANT` can push any literal at any point,
  so the entire constant pool is live for the whole run — not just the values
  currently on the stack.
- **The window between allocating and storing.** A freshly allocated object is
  reachable from nothing until it is written somewhere the collector scans. Any
  allocation in that window frees it. `csPushTempRoot` / `csPopTempRoot` bridge
  it; `csChunkAddConstant` and `allocateString` both need it.

`make test-gc` builds with `CS_DEBUG_STRESS_GC`, which collects on *every*
allocation. If a root is missing, the object dies immediately and the suite
fails rather than the bug surfacing months later under memory pressure.

## Variables: why locals are fast and globals are not

A naive interpreter stores every variable in a hash map keyed by its name, so
reading `i` inside a loop costs a hash, a probe and a string comparison on every
iteration.

CScript resolves scopes **at compile time** instead. The compiler keeps a stack
of the locals currently in scope; when it sees an identifier it walks that list
backwards — which is also what makes an inner declaration shadow an outer one —
and emits `OP_GET_LOCAL` with the stack slot number. At run time that is a
single array index.

Globals still go through a hash lookup, because a global's binding can be
created by code the compiler has not seen yet. That asymmetry is deliberate and
worth knowing when writing hot code: **loop counters and accumulators should be
`let` inside the loop, not globals.**

Two details fall out of the design:

- A declaration inside a block emits *no instruction at all*. The initialiser
  has already left its value exactly where the local's slot is, so declaring it
  is pure compile-time bookkeeping.
- Leaving a scope pops every local it introduced in one `OP_POP_N` rather than a
  run of `OP_POP`s.

## Objects and native functions

`console.log` is not a statement or a special form — it is a property lookup on
a real object that yields a real function value, which `OP_CALL` then invokes.
That is why `typeof console` is `"object"` and `typeof Math.floor` is
`"function"`, and why the same machinery will carry user-defined objects in the
next milestone without redesign.

Three heap types exist today:

| Type | Holds | Marked by the collector as |
| --- | --- | --- |
| `ObjString` | interned characters, stored inline | a leaf — no outgoing references |
| `ObjNative` | a C function pointer and its name | its name |
| `ObjObject` | a property table and a name | its name and every property |

`blackenObject` gained real work here. When it did, the temporary-root discipline
described above stopped being theoretical: `csObjectSetProperty` allocates twice
(interning the key, then possibly growing the table), and both the receiver and
the value have to stay rooted across it.

## Performance

Measured on an Apple M3 Pro, best of seven runs, `bench/run.sh`. The reference
points are the same loop written in Go and in C, both at `-O2`.

| Benchmark | v0.2.0 | now | change |
| --- | ---: | ---: | ---: |
| `loop_arith` | 602 ms | 325 ms | −46% |
| `branches` | 598 ms | 395 ms | −34% |
| `loop_empty` | 317 ms | 203 ms | −36% |
| `globals` | 339 ms | 273 ms | −19% |
| `locals` | 252 ms | 160 ms | −37% |
| `strings` | 260 ms | 260 ms | — |
| **total** | **2368 ms** | **1616 ms** | **−32%** |

`loop_arith` against native code: Go 27 ms, C 26 ms. CScript went from 22× Go to
12× Go. The remaining gap is structural, not a missing tweak — see
[the ceiling](#the-interpreter-ceiling).

### What actually helped

**An integer fast path for `%`** — the largest single win. `OP_MODULO` called
`fmod()` unconditionally; in isolation, ten million `fmod` calls take 182 ms
against 8 ms for integer remainder, a 23× difference. Loop counters are
integers virtually always, so the fast path applies whenever it is exactly
equivalent. A zero dividend is excluded so `-0 % n` stays `-0`.

**In-place local update** — `i++` in statement position had to produce the old
value nobody reads, costing a duplicate and two pops. `OP_INC_LOCAL` does the
whole thing in one instruction with no stack traffic. The canonical loop body
went from 16 instructions to 11.

**An inlined numeric path for `+`**, skipping the call and two string checks
that the general concatenate-or-add path performs.

### What did not help: computed goto

The interpreter dispatches through computed goto where the compiler supports it
and a switch everywhere else. On this hardware the choice is **a wash**:

| Benchmark | computed goto | switch |
| --- | ---: | ---: |
| `locals` | 160 ms | 190 ms |
| `loop_arith` | 325 ms | 342 ms |
| `loop_empty` | 203 ms | 214 ms |
| `globals` | 273 ms | 284 ms |
| `branches` | **395 ms** | **332 ms** |
| total | 1616 ms | 1624 ms |

It wins on four of six — the tight loops, where the same short opcode sequence
repeats and each dispatch site gets a clean branch history. It loses badly on
`branches`, whose control flow is data-dependent and which touches a wider
spread of opcodes: replicating the dispatch sequence at 38 sites costs
instruction-cache footprint, and the per-site predictors each see less history
than the single shared one does.

Computed goto is kept as the default because the loops it helps are more
representative of interpreter work, and because this result is specific to one
ARM64 core — the technique generally wins by more on x86-64, which is not
tested here. `make test-switch` runs the whole suite through the other path so
the fallback cannot rot.

This is the sort of thing the benchmark suite exists to catch. The optimisation
was expected to be worth 15–25% and was worth nothing; without measuring, it
would have been documented as a win.

### The interpreter ceiling

Roughly 12× Go, and the remaining distance is architectural. Go compiles the
benchmark loop to about four machine instructions; CScript executes eleven
bytecode instructions through a dispatch loop, each with a stack round-trip.
Nothing removes that floor.

Ideas that would still help, in rough order of value per effort:

| Idea | Expected | Effort |
| --- | --- | --- |
| Superinstructions (fuse hot opcode pairs) | 20–40% | medium |
| NaN-boxing — `Value` 16 bytes to 8 | 10–20% | medium |
| Inline caches for global and property lookup | 10–20% | medium |
| Register-based bytecode instead of a stack | 20–40% | large |
| A JIT | order of magnitude | very large |

Only the last closes the gap with Go, and it is what makes Node 2.8× rather
than 12×. It is also a multi-year project. A well-tuned bytecode interpreter
lands somewhere around 5–10× native, and CScript is now inside that band.

## Values

`Value` is a 16-byte tagged union: a `ValueType` plus a payload of a `bool`, a
`double`, or an `Obj *`. Nothing outside `value.h` touches the representation —
everything goes through the `IS_` / `AS_` / `_VAL` macros — so NaN-boxing it
down to 8 bytes later is a change to one header.

Strings are immutable, interned, and store their characters inline in the same
allocation as the header. Interning makes equality a pointer comparison and
hashing a field read.

## Error handling

There is one `Diagnostics` object per interpretation. The lexer, parser and
compiler all report through `csDiagnosticError`, which prints the message, the
offending source line and a caret span underneath.

The parser sets `panicMode` on the first error and suppresses further reports
until `synchronize()` reaches a statement boundary. Without it, one stray token
produces a cascade of errors that bury the real one. Errors are counted, so the
driver can report every independent problem in a file in a single run.

Runtime errors print the failing line by looking up `vm.ip` in the chunk's line
table, then reset the stack.

## Source layout

| Path | Holds |
| --- | --- |
| `include/cscript/` | One public header per module |
| `src/lexer.c` | Source text to tokens |
| `src/parser.c` | Tokens to AST, precedence climbing, error recovery |
| `src/ast.c` | Node constructors and the arena |
| `src/compiler.c` | AST to bytecode |
| `src/vm.c` | The interpreter loop |
| `src/memory.c` | The allocator and the collector |
| `src/object.c` | Heap object types, string interning |
| `src/table.c` | Open-addressing hash table |
| `src/value.c` | Value operations, coercion, formatting |
| `src/chunk.c` | Bytecode buffer and constant pool |
| `src/native.c` | The built-in global environment |
| `src/debug.c` | Disassembler and AST printer |
| `src/diagnostic.c` | Error reporting |
| `src/main.c` | CLI, REPL, file runner |

## Build configurations

Each configuration compiles into its own directory under `build/`. Sharing one
would let a release object satisfy a debug build, producing a binary linked
against a sanitizer runtime but compiled without instrumentation — which is
exactly the kind of failure that wastes an afternoon.

| Target | Flags | Use |
| --- | --- | --- |
| `make` | `-O2 -DNDEBUG` | Normal build |
| `make debug` | `-O0 -g3` + UBSan | Development and `make test` |
| `make asan` | debug + AddressSanitizer | Hunting memory errors |
| `make gcstress` | debug + collect on every allocation | Finding missing GC roots |
| `make trace` | debug + all four stage dumps | Understanding the pipeline |

UBSan is in the default debug build because it is portable and cheap, and
`-fno-sanitize-recover` makes undefined behaviour abort rather than warn.
AddressSanitizer is a separate target because it fails to start under some
sandboxed environments, and that should not be able to wedge `make test`.

## What comes next

The pipeline has not changed shape since milestone 1, which was the point of
building it first. Milestone 2 added variables, control flow, calls and objects
without adding a single new stage: the parser grew statement forms, the compiler
grew scope resolution, the VM grew opcodes, and the collector grew two object
types.

| Milestone | Adds | Touches |
| --- | --- | --- |
| **2 ✅** | **`let`/`const`, scopes, control flow, calls, `console.log`** | — |
| 3 | User functions, call frames, closures | VM gains a frame stack; GC gains upvalues |
| 4 | Object literals, arrays, indexing | `ObjObject` already exists; add `ObjArray` |
| 5 | `switch`, `break`/`continue`, `for...of` | parser and compiler only |
| 6 | Template literals, ternary, destructuring | parser and compiler only |
| 7 | NaN-boxing, computed-goto dispatch | `value.h` and the VM loop only |
