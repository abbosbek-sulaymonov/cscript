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
  ┌─────────┐   AST + types         src/typecheck.c    CS_DEBUG_PRINT_AST
  │ Checker │   annotates every node in place
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

## The type checker

A pass between parsing and code generation. It walks the tree, resolves a type
onto every expression node, and reports mismatches through the same diagnostics
object every other stage uses.

Two things come out of it. The obvious one is errors the programmer sees before
the program runs. The less obvious one is that `resolvedType` stays on the AST,
so the compiler can specialise against it — the types are **consumed, not
erased**. `a + b` where both sides resolved to `number` emits `OP_ADD_NUM`,
which does no type dispatch; the same expression on `any` values emits the
generic `OP_ADD`.

The checker keeps its own scope stack rather than sharing the compiler's. That
duplication is deliberate: it means the compiler can be changed without silently
altering what is or is not a type error.

### Why static types are worth ~2% for speed

Before building any of this, a variant was compiled with **every runtime type
check deleted** — the theoretical maximum a perfect type system could deliver in
this architecture. It was 2.2% faster in total. `OP_ADD_NUM` subsequently
measured 2.5%, which matched.

The checks cost almost nothing because they always go the same way, so the
branch predictor gets them right every time. The bottleneck is dispatch and
16-byte stack traffic, and an annotation touches neither.

That is the honest case for gradual typing here: it is worth doing **for
correctness and tooling**, and the speed argument only becomes real one step
later, when the types are used to change *representation* rather than to skip a
predictable branch:

- **Unboxing** — a `number` local in a raw 8-byte slot instead of a 16-byte
  tagged `Value`. This attacks memory traffic, which is the actual bottleneck.
- **Type-specialised superinstructions** — fusing typed operations.
- **A JIT emitting native code with no guards**, which is where Go's advantage
  actually lives.

`OP_ADD_NUM` exists mostly to prove the pipeline end to end: an annotation
changes what the checker knows, which changes what the compiler emits, which
changes what the VM runs.

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

Globals cannot be resolved that way, because a global's binding can be created
by code the compiler has not seen yet. They are instead resolved *once* and
then remembered: each global site carries an inline cache holding the table
entry it found, alongside the version the table had at the time. A rehash or a
delete bumps the version and the site looks the name up again; nothing else
can move an entry, so in the ordinary case — a global declared before it is
used and never removed — the cache holds for the life of the program and the
instruction is a version compare and a load.

That still leaves globals behind locals, because a local is an array index with
nothing to compare. The advice stands, if less strongly than before: **loop
counters and accumulators should be `let` inside the loop.**

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

`blackenObject` gained real work here. When it did, the temporary-root
discipline described above stopped being theoretical: `csObjectSetProperty`
allocates twice (interning the key, then possibly growing the storage), and both
the receiver and the value have to stay rooted across it.

The standard library is **frozen** once it is built. `Math.PI = 3` and
`console.log = f` are errors at the line that writes them. This is a deliberate
divergence: in JavaScript both succeed, and the failure surfaces somewhere else
entirely. User objects are never frozen, including one whose key happens to
match a built-in name.

## Shapes: how an object stores a property

An object could be a hash table, and was one until v0.13.0. But the same
literal written inside a loop produces a million objects with identical
layouts, and the name being looked up was known when the code was compiled.

A **shape** captures the layout once and shares it. The object holds nothing
but a flat array of values; the shape says which index each name lives at.
Shapes form a transition tree, and adding the same key to the same parent
always finds the same child, so `{}`, `{x}` and `{x, y}` are three nodes no
matter how many objects walk the path:

```
root ──x──▶ {x} ──y──▶ {x,y}
     └──a──▶ {a}
```

Insertion order — which JavaScript guarantees for string keys, and which
anything that prints or serialises an object depends on — is slot order, so it
costs nothing to preserve.

Three things had to be got right, and each is a place the design could have
quietly failed:

- **The transition edges are weak.** A parent does not keep a child alive.
  Otherwise the root, which is a permanent VM root, would pin every layout any
  program ever built. The collector prunes edges to shapes nothing else
  references — which in turn means a shape freshly returned from a transition
  is held by nothing until an object adopts it, and must be rooted across any
  allocation in between. Missing that is a use-after-free that only `test-gc`
  finds.
- **Publication order.** The collector sizes its walk of an object's slots from
  the shape, so an object keeps its old shape until the new slot actually holds
  a value; and a shape keeps `slotCount` at zero until its key array exists.
  Every line in between can collect.
- **Dictionary mode.** A shape chain costs O(n²): each node copies its parent's
  index map and key list to gain one entry. That is right for the handful of
  fields a record has and a disaster for an object filled key by key in a loop.
  Past 64 properties an object converts to a plain hash table, once, for good.
  Inline caches simply miss on it.

## Classes on top of shapes

Classes were built after shapes on purpose, and the payoff is how little of the
runtime they needed.

**An instance is an `ObjObject` with a class pointer.** Not a new heap type. So
a field is a slot, every instance of a class shares one shape the way every
object from one literal does, and inline caches, `Object.keys`, `JSON.stringify`
and the GC walk all worked on instances before instances existed. What a class
adds is a fallback: a property that misses the shape is looked up along the
superclass chain.

**Methods are not copied into subclasses.** `csClassFindMethod` walks the chain.
Copying would make lookup one probe instead of a short walk, at the cost of a
method added to a base class no longer being visible from its subclasses. The
chains are short and the walk is pointer compares.

**`this` is slot 0.** A call already leaves the receiver directly below the
arguments, which is exactly where a frame's slot 0 lands — every other call path
overwrites that slot with the callee, and a method simply does not. The compiler
then names slot 0 `this`, so an ordinary local lookup finds it, and an arrow
function inside a method captures it through the upvalue machinery. JavaScript's
lexical `this` for arrows falls out with no rule of its own.

**`super` is a hidden local.** The superclass is left on the stack for the
length of the class body, and a method mentioning `super` captures it as an
upvalue. That is what makes `super` resolve against the class a method was
*written* in rather than the class of the receiver — the difference that matters
the moment a hierarchy is three deep.

**A constructor returns `this`**, which the compiler arranges by emitting
`OP_GET_LOCAL 0` where it would otherwise emit `OP_UNDEFINED`. So `OP_NEW` needs
no instruction to recover the instance: the frame leaves it behind.

### Where field initialisers run

This is the part that took the most care, because getting it wrong is visible.

JavaScript runs a class's field initialisers at the point its own constructor
begins: at the top of the body for a base class, and directly after `super(...)`
returns for a derived one. Run them all up front instead and `Object.keys`
comes back in a different order — CScript went to some trouble to preserve
insertion order elsewhere, so getting it wrong here would be inconsistent.

So a class with a constructor has its field assignments **compiled into that
constructor**, spliced in at the right point. Nothing extra runs at all. To keep
the splice point unambiguous, a subclass constructor must call `super(...)` as
its first statement — stricter than JavaScript, which only requires it before
the first use of `this`.

A class with *no* constructor keeps its fields in a hidden method the VM calls
where the implicit constructor would have. Almost always that is immediately,
before the inherited constructor; the exception is a class that declares fields,
declares no constructor, and inherits one — there the constructor has to finish
first, so it runs in a nested interpreter loop and the fields follow it. That
shape is rare enough to be worth the slow path and common enough to be worth
getting right.

## Inline caches

Both property and global sites carry a cache, stored in a side array on the
chunk rather than patched into the bytecode. The collector has to find every
cached shape, and walking a flat array is much safer than decoding instructions
to locate operands.

A property cache holds a shape and a slot. The hit test is one pointer compare;
on a hit the property is an indexed load.

The caches are **monomorphic** — one entry per site, replaced whenever a
different shape arrives. A site that genuinely sees many shapes falls back to
the hash lookup, which is exactly what it cost before caches existed, so the
worst case is no worse.

Two details that are not optional:

- An empty cache cannot be represented by a null shape, because that is what a
  dictionary-mode object's shape is — an unfilled cache would report a hit on
  the first dictionary object to reach it and then index a slot array that no
  longer exists. A sentinel shape that no object is ever given keeps the hit
  test at a single compare.
- The fast path for assigning to a property skips the frozen check, which is
  sound only because a write to a frozen object errors before that site ever
  gets to fill its cache.

Three heap types became six:

| Type | Holds | Marked by the collector as |
| --- | --- | --- |
| `ObjString` | interned characters, stored inline | a leaf — no outgoing references |
| `ObjNative` | a C function pointer, its name, optional statics | its name and statics |
| `ObjObject` | a shape and a flat slot array, or a table | its name, shape and live slots |
| `Shape` | a layout: parent, key, index map, transitions | parent, key and keys — *not* transitions |
| `ObjArray` | a dense `ValueArray` | every element |
| `ObjFunction` | a chunk, its constants and its caches | its name and constant pool |

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

### What did help: superinstructions

The first optimisation chosen from a profile rather than from intuition, and the
first to deliver what it promised.

A build with `CS_DEBUG_PROFILE_OPCODES` counts how often each opcode follows
each other opcode. Running the benchmark suite through it named the candidates
directly:

| Pair | Share of instructions | Why it happens |
| --- | ---: | --- |
| `OP_GET_LOCAL` → `OP_CONSTANT` | 14–18% | `i < n`, `i % 7`, `total + 1` |
| `OP_SET_LOCAL/GLOBAL` → `OP_POP` | 14% | an assignment used as a statement |
| comparison → `OP_POP_JUMP_IF_FALSE` | 8–11% | every loop condition |

Each became one instruction:

- **`OP_SET_LOCAL_POP` / `OP_SET_GLOBAL_POP`** — a statement's assignment stores
  without writing back a value nothing reads.
- **Fused compare-and-branch** — `OP_JUMP_IF_NOT_LESS` and its five siblings
  test the operands directly, so a loop condition no longer materialises a
  boolean only to pop it one instruction later.
- **`OP_GET_LOCAL_CONST`** — pushes a local and a literal together.

All three are emitted by pattern-matching the AST, not by a peephole pass over
finished bytecode. That matters: a peephole pass would have to find and fix
every jump offset that straddles a fused pair, and getting that wrong produces
bugs that only appear in branchy code.

The canonical loop body went from 16 instructions to 7:

```
OP_GET_LOCAL_CONST  i, 1        ; i < 10000000
OP_JUMP_IF_NOT_LESS -> exit
OP_GET_GLOBAL       s
OP_GET_LOCAL_CONST  i, 7        ; i % 7
OP_MODULO
OP_ADD_NUM
OP_SET_GLOBAL_POP   s
OP_INC_LOCAL        i
OP_LOOP
```

| | before | after |
| --- | ---: | ---: |
| instructions executed | 130,000,015 | 90,000,013 |
| `loop_arith` | 335 ms | 265 ms |
| `locals` | 231 ms | 191 ms |
| `branches` | 423 ms | 319 ms |
| **total** | **1835 ms** | **1565 ms** |

31% fewer instructions for 15% less time — which is itself informative. If
dispatch were free, cutting instructions would buy nothing; if dispatch were
everything, the two numbers would match. It sits in between.

### What did not help: NaN-boxing (for speed)

A `Value` is 8 bytes rather than 16 where the platform allows it: IEEE 754
leaves roughly 2^51 bit patterns that all mean "not a number", and every
non-number value hides in there. The expectation was 10–20%.

The measured speed difference is **zero**:

| Benchmark | NaN-boxed (8B) | tagged union (16B) |
| --- | ---: | ---: |
| `arrays` | 94 ms | 98 ms |
| `loop_arith` | 335 ms | 345 ms |
| `globals` | 266 ms | 286 ms |
| `branches` | 423 ms | 396 ms |
| total | 1835 ms | 1831 ms |

What it does buy is **memory**, which the timings do not show at all:

| Program | NaN-boxed | tagged union |
| --- | ---: | ---: |
| 200k-element array | 3.1 MB | 6.7 MB |
| 1M-element array | 11.3 MB | 18.9 MB |

So it stays on — halving the footprint of every array, object and stack slot is
worth having — but it is filed as a memory optimisation, not a speed one.
`make test-tagged` runs the whole suite through the other representation.

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

### What the measurements add up to

| Optimisation | Predicted | Delivered |
| --- | --- | --- |
| Computed-goto dispatch | 15–25% | 0% |
| NaN-boxing | 10–20% | 0% time, 45% memory |
| Removing every type check | large | 2% |
| **Superinstructions** | **20–40%** | **15%** |

The first three all attacked the cost of *executing* an instruction, and all
three found it already close to free — modern branch predictors and caches had
absorbed it. The fourth attacked the *number* of instructions, and worked.

The difference between the first three and the fourth is not sophistication. It
is that the fourth was chosen from a profile. Three guesses cost real work and
returned nothing; one measurement named the exact pairs worth fusing and the
result landed inside its predicted range.

That is the argument for `bench/` and for the opcode profiler, and it is why the
remaining ideas below are hypotheses rather than plans.

### The interpreter ceiling

Roughly 10× Go, and the remaining distance is architectural. Go compiles the
benchmark loop to about four machine instructions; CScript executes eleven
bytecode instructions through a dispatch loop, each with a stack round-trip.
Nothing removes that floor.

Ideas that would still help, in rough order of value per effort:

| Idea | Hypothesis | Effort |
| --- | --- | --- |
| Register-based bytecode instead of a stack | 20–40% | large |
| Inline caches for globals and properties | 10–20% | medium |
| More superinstructions from the profile | diminishing | small |
| A JIT | order of magnitude | very large |

The globals benchmark is still 3.7× Node and every global access is a hash
lookup, so an inline cache is the next thing worth profiling.

Given the record above, treat those percentages as hypotheses to be measured
rather than as savings already in hand.

Only the last closes the gap with Go, and it is what makes Node 2.8× rather
than 12×. It is also a multi-year project. A well-tuned bytecode interpreter
lands somewhere around 5–10× native, and CScript is now inside that band.

## Number formatting

`Number::toString` is one of the most visible parts of a language, and C's `%g`
does not implement it. The two disagree about when to use exponent notation —
C switches once the exponent leaves `[-4, precision)`, JavaScript once the
decimal point would fall outside `(-6, 21]` — and printf pads the exponent to
two digits, writing `1e-07` where JavaScript writes `1e-7`.

`src/value.c` implements the ECMA-262 rule directly: find the shortest digit
string that reads back as the same double, then place the decimal point. It is
checked against Node over 227 hand-picked forms and 400 random doubles.

The display form and the string-conversion form are kept apart, because
JavaScript distinguishes them too: `console.log(-0)` prints `-0` while
`String(-0)` returns `"0"`, and a string nested inside a container is quoted
while a bare one is not.

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
| `src/typecheck.c` | Static checking; annotates the AST with types |
| `src/type.c` | The type lattice and assignability |
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
| **3 ✅** | **Gradual typing: annotations, inference, checking** | — |
| 4 | Unboxed typed locals — where typing pays off in speed | `Value`, VM, compiler |
| 5 | User functions, call frames, closures | VM gains a frame stack; GC gains upvalues |
| 4 | Object literals, arrays, indexing | `ObjObject` already exists; add `ObjArray` |
| 5 | `switch`, `break`/`continue`, `for...of` | parser and compiler only |
| 6 | Template literals, ternary, destructuring | parser and compiler only |
| 7 | NaN-boxing, computed-goto dispatch | `value.h` and the VM loop only |
