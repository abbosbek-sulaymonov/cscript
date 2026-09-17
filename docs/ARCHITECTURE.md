# CScript architecture

CScript runs a script in four stages. Each one has a single job, its own header
in `include/cscript/`, and a debug flag that dumps what it produced.

```
  source text
      │
      ▼
  ┌─────────┐   Token stream        src/compiler/lexer.c        CS_DEBUG_PRINT_TOKENS
  │  Lexer  │   borrows the source, copies nothing
  └─────────┘
      │
      ▼
  ┌─────────┐   AST                 src/compiler/parser.c       CS_DEBUG_PRINT_AST
  │ Parser  │   arena-allocated, freed in one call
  └─────────┘
      │
      ▼
  ┌─────────┐   AST + types         src/compiler/typecheck.c    CS_DEBUG_PRINT_AST
  │ Checker │   annotates every node in place
  └─────────┘
      │
      ▼
  ┌─────────┐   Chunk               src/compiler/compiler.c     CS_DEBUG_PRINT_CODE
  │Compiler │   bytecode + constant pool + line table
  └─────────┘
      │
      ▼
  ┌─────────┐   Values              src/runtime/vm.c           CS_DEBUG_TRACE_EXECUTION
  │   VM    │   stack machine
  └─────────┘
```

## Where things live

```
src/compiler/   lexer, parser, checker, bytecode compiler, and the disassembler
src/runtime/    the VM, objects, values, memory, and the regex engine
src/native/     the standard library, one file per built-in
src/jit/        lowering to typed IR, and the arm64 backend
src/main.c      the entry point
include/cscript/  every public header, one per module above
library/        the standard library, written in CScript itself
```

The grouping is by role rather than by stage, which is why `native/` is beside
`runtime/` rather than inside it: a built-in is written against the VM's public
surface, and keeping the two apart is what stops that surface from quietly
growing. A file in one group names a header in another through a path from
`src/` — `runtime/vm_internal.h` — so the direction of a dependency is visible
at the include rather than in a chain of dots.

`library/` is the one part of the implementation that is not C. Twenty-four
modules written in the language, one directory each — `library/iter/iter.cx`
beside the README that specifies it — imported as `std:name` and resolved
against the
binary's own location rather than the importing file, because a script
anywhere on disk has to mean the same file by `std:iter`. It is checked
against Node the way the language is: `tests/std_node.sh` rewrites the `std:`
specifiers to relative paths so Node can run the same program, and every
expected file in `tests/cases/stdlib/` came out of that.

Each of those directories has a README mapping its own files —
[`src/README.md`](../src/README.md) is the way in, and the four beside it go
into the groups within each. This document is the *why*; those are the *where*.

Test cases are grouped the same way, under `tests/cases/`: `language`,
`library`, `types`, `async`, `imports`, `errors` and `jit`. A directory holding
a `main.cx` is a case that spans files; anything else is a group to look
inside, which is the whole of what the runner needs to know.

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
which does no type dispatch; the same expression on values the checker could
not resolve — an array's elements, an object's properties — emits the generic
`OP_ADD`.

The lattice is flat, and two of its members carry the weight of the design.
`unknown` is the top type a program can **write** — TypeScript's own name for
it: everything is assignable to one, nothing is assignable from one, and
nothing may be read off one until a `typeof` has said what it holds. Beside it sits a type that cannot be written
at all — the checker's own "I could not tell", which flows both ways and is
where the runtime does the checking instead. Keeping the two apart is what
makes `unknown` a type rather than an escape hatch: `any` would be the second
one with a name, and naming it is what lets it spread.

Types are no longer flat. A type is an **int**: below a fixed base it is one of
the primitive kinds, and from there on it indexes the file's table, where the
composite ones live — an array of something, a function from something to
something, a union of several, a declared shape, a type variable. Composites
are interned, so `number[]` written twice is one entry and one id, and
comparing two types is usually an integer compare with structural equality
falling out of construction.

That encoding is what keeps the cost where it was — so a type stays
one scalar passed by value through the whole checker and stored in a byte on a
compiled function. A type *tree* is what this becomes when generics or unions
need one, and deliberately not before. The table hangs off the program node
rather than sitting in a global, because a module is parsed and checked in the
middle of the compile of the module that imported it.

It outlives the parse, though, and deliberately: the loader reads a file's
dependencies *before* checking it, so by the time a file is checked every
module it imports has a settled table of its own, handed to the module object
when its arena went. Importing a type is then re-interning one table's entry
into another's — which works only because the whole relation is structural. A
shape written in two files is one type, and neither has to import the other's
name for that to hold.

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

That is the honest case for static typing here: it is worth doing **for
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

**The AST arena** (`src/compiler/ast.c`) bump-allocates from 64 KB blocks and frees
everything at once. AST nodes are small, numerous, and all die at the same
instant, so per-node bookkeeping would be pure overhead.

**The garbage-collected heap** (`src/runtime/memory.c`) owns everything that outlives
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

### Weak references, and the loop one of them needs

Three things are held weakly, and all three are resolved in one pass after
marking is final and before anything is freed: a shape's transition edges, a
chunk's inline caches, and a `WeakMap`'s keys. Caching a layout, or recording a
route to one, or looking something up by it, must not be what keeps it alive.

A `WeakMap` needs more than not-marking. Its *values* have to be marked, but
only for the keys that turned out to be live — and a value may itself be the
only thing keeping another map's key alive. So the pass repeats until nothing
new is marked, which is what makes the answer independent of the order the maps
happen to sit in. Three links of that chain are in the test, because one
marking pass gets two of them right and looks convincing.

Nothing may allocate during a collection, so pruning clears the dead entries in
place and rebuilds the index over what is left rather than calling the ordinary
rehash.

A weak collection also refuses to be iterated, spread, counted or printed with
its contents. Every one of those would let a program work out when the
collector last ran.

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

That still leaves globals behind locals in the interpreter, because a local is
an array index with nothing to compare. The advice stands, if less strongly
than before: **loop counters and accumulators should be `let` inside the loop.**

Inside *compiled* code the gap closes completely: the entry's address is baked
in and a global costs one load, the same as a local. What makes that safe is in
[Stage 5](#globals-and-what-makes-an-address-safe-to-bake).

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

| Measure | Before | After |
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

`src/runtime/value.c` implements the ECMA-262 rule directly: find the shortest digit
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
| `include/cscript/` | One public header per subsystem |
| **Front end** | Source text to a tree |
| `src/compiler/lexer.c` | Source text to tokens |
| `src/compiler/lexer_keyword.c` | Which identifiers are keywords, and every token's name |
| `src/compiler/lexer_internal.h` | The seam between those two |
| `src/compiler/parser.c` | The token plumbing, the precedence table, and `csParse` |
| `src/compiler/parser_expression.c` | Precedence climbing, and the primary dispatcher |
| `src/compiler/parser_primary.c` | The operands an expression can start with |
| `src/compiler/parser_prefix.c` | The operators an expression can start with |
| `src/compiler/parser_arrow.c` | Templates, arrows, and the function forms |
| `src/compiler/parser_declaration.c` | Variable declarations and the patterns they bind |
| `src/compiler/parser_class.c` | The class body, in source order |
| `src/compiler/parser_module.c` | `import` and `export`, in every form |
| `src/compiler/parser_statement.c` | Blocks, conditionals, the loop forms, `switch`, `try` |
| `src/compiler/parser_internal.h` | What all of those share |
| `src/compiler/ast.c` | The arena, and the expression nodes |
| `src/compiler/ast_statement.c` | The statement and declaration nodes |
| `src/compiler/ast_name.c` | An operator's name, for diagnostics and the dump |
| `src/compiler/ast_internal.h` | How a node and a list are made |
| **Checking** | The types, before any code is generated |
| `src/compiler/typecheck.c` | The scope, the builtins, and the node dispatcher |
| `src/compiler/typecheck_value.c` | The type of an expression |
| `src/compiler/typecheck_statement.c` | The checking a statement needs |
| `src/compiler/typecheck_internal.h` | The checker's state |
| `src/compiler/type.c` | The type lattice and assignability |
| **Back end** | The tree to bytecode |
| `src/compiler/compiler.c` | Emit helpers, scopes, locals, and the node dispatcher |
| `src/compiler/compiler_node_value.c` | What each expression node becomes |
| `src/compiler/compiler_node_statement.c` | What each statement node becomes |
| `src/compiler/compiler_expression.c` | Operators, and the conditions that feed a jump |
| `src/compiler/compiler_assign.c` | Assignment, in all the forms it takes |
| `src/compiler/compiler_function.c` | Functions, and what `this` means inside one |
| `src/compiler/compiler_statement.c` | Control flow and the destructuring a declaration lowers to |
| `src/compiler/compiler_class.c` | Classes: members, accessors, statics, constructors |
| `src/compiler/compiler_module.c` | Imports and exports, resolved at compile time |
| `src/compiler/compiler_internal.h` | The compiler's ambient state and the seams |
| `src/compiler/chunk.c` | Bytecode buffer, constant pool, inline-cache arrays |
| **Runtime** | Running the bytecode |
| `src/runtime/vm.c` | The interpreter: the dispatch, and the unit everything below is compiled into |
| `src/runtime/vm_state.inc` | The one VM, and the stack it runs on |
| `src/runtime/vm_number.inc` | Errors, and the arithmetic that has to be exact |
| `src/runtime/vm_property.inc` | Accessors, and asking an object for what it may not have |
| `src/runtime/vm_call.inc` | Calling, and the arity a call has to satisfy |
| `src/runtime/vm_invoke.inc` | A callee that is not a plain closure |
| `src/runtime/vm_iterate.inc` | Object keys, and what `for...of` pulls from |
| `src/runtime/vm_upvalue.inc` | Captured locals, and the opcode profile |
| `src/runtime/vm_throw.inc` | Where a throw goes |
| `src/runtime/vm_cache_miss.inc` | What an inline cache misses |
| `src/runtime/vm_ops_*.inc` | The opcode bodies, by role — six files |
| `src/runtime/vm_fiber.c` | Suspendable calls, for `await` |
| `src/runtime/vm_event.c` | Microtasks, timers, and the loop that drains them |
| `src/runtime/vm_internal.h` | The seams between those three |
| `src/runtime/object.c` | The allocation every heap object goes through, and strings |
| `src/runtime/object_internal.h` | The one thing the object files share |
| `src/runtime/object_bag.c` | An object's properties: slots, then a table |
| `src/runtime/object_new.c` | The constructors for everything that is not a property bag |
| `src/runtime/object_print.c` | How each object prints, which is not how it converts |
| `src/runtime/object_gc.c` | What the collector does with each object type |
| `src/runtime/shape.c` | Hidden classes: the layout an object has |
| `src/runtime/memory.c` | The allocator and the collector |
| `src/runtime/table.c` | Open-addressing hash table |
| `src/runtime/value.c` | What a Value is, and what it converts to |
| `src/runtime/value_render.c` | Turning a Value into text, in the two ways that differ |
| `src/runtime/value_internal.h` | The seam between those two |
| `src/runtime/module.c` | Resolving, loading and ordering source files, `std:` included |
| `src/runtime/bigint.c` | Arbitrary-precision integers |
| `src/runtime/regex.c` | Compiling a pattern to a program |
| `src/runtime/regex_match.c` | Running one against a subject |
| `src/runtime/regex_internal.h` | The compiled program both halves see |
| **Compiler (second tier)** | Bytecode to machine code |
| `src/jit/jit.c` | Tiering: what gets hot, and what happens when it does |
| `src/jit/ir.c` | The walk that lowers a function's bytecode |
| `src/jit/ir_internal.h` | The lowering's own state, and the seams between its files |
| `src/jit/ir_build.c` | Registers, instructions, block boundaries, push and pop |
| `src/jit/ir_lower_data.c` | Constants, globals, locals and the operand stack |
| `src/jit/ir_lower_arith.c` | Arithmetic and comparison |
| `src/jit/ir_lower_object.c` | Property reads and writes, and the layouts they assume |
| `src/jit/ir_lower_flow.c` | Jumps, branches, calls and returns |
| `src/jit/ir_inline.c` | Splicing a small callee's body in where the call was |
| `src/jit/ir_replay.c` | What the interpreter does across a hand-over |
| `src/jit/ir_types.c` | What each value holds, and whether it is proved enough to run |
| `src/jit/ir_print.c` | The IR in readable form, and where its typing stops |
| `src/jit/ir_interpret.c` | Running the lowered form, to check it against the bytecode |
| `src/jit/jitcode.c` | Compiling a lowered function, and the memory it runs from |
| `src/jit/jitcode_internal.h` | The seam between the backend's files |
| `src/jit/jitcode_arm64.c` | The arm64 instructions this backend can emit |
| `src/jit/jitcode_alloc.c` | Deciding where every value lives |
| `src/jit/jitcode_emit.c` | One IR instruction to machine code |
| **Standard library** | One namespace per file |
| `src/native/native_fs.c` | `fs`: read, write, stat, list — answers, never throws |
| `src/native/native_process.c` | `process`: arguments, environment, directory, platform, exit |
| `src/native/native.c` | The global environment, and what is installed into it |
| `src/native/native_internal.h` | The seam between the library's files |
| `src/native/native_object.c` | The `Object` namespace |
| `src/native/native_descriptor.c` | Property descriptors, both directions |
| `src/native/native_math.c` | `Math`, and the numeric functions |
| `src/native/native_convert.c` | Converting to a number, string or boolean, and parsing |
| `src/native/native_array.c` | Array methods that stay inside the runtime |
| `src/native/native_array_callback.c` | Array methods that call back into user code |
| `src/native/native_string.c` | String methods that read or reshape |
| `src/native/native_string_search.c` | String methods that take a pattern |
| `src/native/native_number.c` | `Number`, and the numeric conversions |
| `src/native/native_json.c` | `JSON.stringify` and `JSON.parse` |
| `src/native/native_promise.c` | Promises and timers |
| `src/native/native_function.c` | `call`, `apply`, `bind` |
| `src/native/native_generator.c` | Driving a paused call |
| `src/native/native_map.c` | `Map`, `Set`, `WeakMap`, `WeakSet` |
| `src/native/native_date.c` | `Date`, its accessors and its setters |
| `src/native/native_regex.c` | `RegExp`, and the string methods that take one |
| `src/native/native_symbol.c` | `Symbol` and the well-known ones |
| `src/native/native_bigint.c` | The `BigInt` surface |
| **Tools** | Looking at what happened |
| `src/compiler/debug.c` | The disassembler, and the flags that ask for a dump |
| `src/compiler/debug_ast.c` | The parse tree, printed |
| `src/compiler/diagnostic.c` | Error reporting |
| `src/main.c` | The command line, the REPL, and the file runner |

### How it was split, and what was not

Three files held 41% of the code: `vm.c`, `compiler.c` and `parser.c`. They
were split by **what they handle**, not by phase — the pieces of a
recursive-descent parser are mutually recursive because the grammar is, so
layering them would have been a fiction. Each group shares an internal header
that nothing outside it includes.

**`vm.c` could not be split that way at all**, and finding out why is worth
recording. Its opcode bodies each end in `VM_NEXT()`, which under
computed-goto dispatch is a jump to a label in `run()` — so a case cannot
become a function without giving up the dispatch strategy
[measured above](#what-did-not-help-computed-goto). And its helpers *could*
become separate translation units, but doing so cost **13-49%** on the
interpreter benchmarks — 49% on `bench/branches.cx` — because the compiler had
been inlining and specialising them into the loop, and a cross-unit call
cannot be inlined.

So the file is split and the translation unit is not: `vm.c` is a list of
`#include`s of `.inc` fragments, and the object code is byte-for-byte what it
was. An interleaved A/B of the two binaries puts the difference between -1.9%
and +2.7%, which is the machine's noise. `vm_fiber.c` and `vm_event.c` stay
separate units because `await` and the microtask queue are not on the hot
path.

`ir.c` was split the same way later, and needed one thing the others did not.
Its cases said `goto handOver` and `goto failed`, which a function cannot do to
its caller, so the four groups answer a `LowerResult` the walk acts on instead
— with a fourth value, `LOWER_UNHANDLED`, that is not an outcome at all. It
means "not mine", so the walk asks each group in turn and what happens to an
opcode none of them claims is the hand-over the `default` case used to do.
Every case body moved unchanged.

**The interpreter loop was not split, and that is deliberate.** Computed-goto
dispatch depends on the labels and the cached `ip` and `frame` living in one
function, and the helpers it calls on every instruction have to stay in the
same translation unit to be inlined. Splitting it would cost exactly the speed
the dispatch exists for. What could come out did: fibers, which run only when
an async call suspends, and the event loop, which runs only after the program
does. Benchmarks were flat across the whole change, which is what said the line
was drawn in the right place.

## Measuring

One rule, learned the hard way: **measure the measurement first.**

`bench/run.sh` spent most of this project calling `python3` twice per
repetition to read the clock, which put that interpreter's own ~19 ms startup
inside every number it produced. Node's 37 ms of process startup did the same
from the other side. Together they reported CScript as 1.4–5.1× slower than
Node when the real figure on compute is 8–34×.

A constant offset is worse than noise. Noise is visible and averages out; an
offset silently flatters whichever side is slower, because it is a smaller
share of a bigger number. Every relative claim built on that harness was wrong
in the same direction.

The harness now times inside one process and reports startup separately, so
both true things are visible: CScript starts about 18× faster than Node, and
its interpreter loop is about 25× slower.

The same rule applies to correctness harnesses. `make test-jit` reports how
many calls, loops and exits the compiler actually took, because a sweep that
agrees on 121 programs the compiler declined to touch is not evidence of
anything — see [Stage 5](#six-soundness-bugs-and-the-harness-that-found-them).

## Tiering: what a JIT would compile

At this stage there was no code generator yet — only the thing that has to
come before one: a count of what gets hot, and a verdict on how much of it
could be compiled without guarding every operation. The backend that followed
starts at [The typed IR](#the-typed-ir).

`make jit` builds it; `CS_JIT_REPORT=1` asks for the report, and
`CS_JIT_DUMP_IR=1` adds the lowered form.

```
  function                        hotness    typed  generic  verdict
  dist                             200000        3        0  compilable
  untyped                          200000        1        2  compilable

  3 of 3 compilable without falling back to the interpreter
  6 of 9 arithmetic sites (67%) have both operand types known
```

**Why this measurement and not another.** The obvious first JIT is a template
JIT — one machine-code stub per opcode, concatenated — which removes dispatch
and nothing else. The opcode-pair work priced that: removing an instruction
here buys about a third of an instruction, because each one already does real
work. A template JIT would be worth 10–15% for an assembler, a code cache and
a new class of bug.

The win has to come from deleting the work *inside* instructions — the box
tests, the cache probes, the guards. And CScript has something a JavaScript
engine cannot have for that: **declared types**. `function dist(x: number, y:
number): number` is a proof, not a guess, so a compiler can emit unboxed
arithmetic with no guard and no deoptimisation point. V8 must speculate,
because JavaScript promises nothing about a value until it sees one.

That is why the profile counts typed against generic sites rather than
instructions. `dist` above is 3 of 3 — every operation in it could be compiled
with no guard at all. `untyped` is 1 of 3, and the one is instructive: `*`
yields a number whatever its operands are, so the checker proves the outer `+`
numeric even with nothing annotated.

**What the counter costs, measured before it was switched on.** A back-edge
counter is two loads, an increment and a compare on every iteration of every
loop: 4.8% on `loop_arith`, 2.8% on `locals`. That is a fair price for a
compiler that pays it back and no price worth paying for instrumentation, so
it is compiled only into the `jit` configuration. Release is byte-identical in
speed — measured interleaved to cancel drift, −0.1%.

The number is kept because it is an input to the decision rather than a
footnote: **a code generator has to beat about 5% on tight loops before it is
even break-even.**

The refusal list in `src/jit/jit.c` is what a *first* backend would leave to the
interpreter — anything that suspends a frame, unwinds past one, or builds a
class. Everything else is arithmetic, moves and branches.

## The typed IR

Between the bytecode and any future machine code sits a typed intermediate
form. `make jit` builds it, `CS_JIT_DUMP_IR=1` prints it:

```
  ir for dist: 1 block, 16 registers, 6 slots
      r0  :num  = load    slot1
      r3  :num  = load    slot3
      r4  :num  = mul     r3, r2
      r12 :num  = add     r11, r10
                    return  r13
```

Three things about it are decisions rather than defaults.

**Not SSA.** Locals stay in numbered slots; only values get names. Phi nodes
are needed for optimisations that reason across a loop, and nothing here does
that yet — building the dominance machinery before there is a backend to use
it would be writing for an imagined future.

**The IR mirrors the frame exactly.** This looked wasteful and turned out to be
required. `let total = 0` emits *no instruction at all*: the initialiser leaves
its value on the stack and the compiler calls that position a local from then
on. The operand stack and the locals are one array. A model with separate slots
and temporaries loses every local that was declared rather than stored — which
is what the first attempt did, and it produced a wrong number rather than a
crash.

**Types come from the bytecode, not a second analysis.** `OP_ADD_NUM` exists
exactly where the checker proved both operands numeric, and the other
arithmetic opcodes require numbers by definition.

### How the lowering is verified

By *replacing* the interpreter with it. Where the IR covers a whole function,
`make test-ir` runs it instead of the bytecode and requires all 79 golden cases
to be unchanged. A mistranslation is a failing test, not a number that is
quietly wrong somewhere.

That found four bugs the eye did not: the frame base being clobbered because
pushes started at slot 0 rather than above the arguments; block entry heights
taken from linear order instead of from the jumps that reach them; a frame slot
being used as a value; and the substitution being done inside `callClosure`,
whose contract is that a frame *has been pushed* — `csVMRunBody` starts an
interpreter loop straight afterwards on the strength of it, so the entry call
segfaulted on a program consisting of one comment.

**Then it found a fifth by counting.** With the gate set to "every value must
be typed", the suite passed — and zero calls had been substituted. A
verification that never runs proves nothing. The counter is now part of the
report for that reason.

The reason nothing ran was the useful finding: **parameter types stopped at the
compiler.** An annotation the checker had proved never reached the run time, so
every argument read as unknown and no function was ever fully typed. Carrying
them onto ObjFunction took 79% of a hot function's values from untyped to
typed, and is what makes a type-directed backend possible at all.

The gate is now precise: a function runs from its IR when every operand of
every *arithmetic* operation is proved to be a number. Equality is excluded —
it is defined for all types and needs no proof. The unreachable `return
undefined` the compiler appends to every function is excluded for the same
reason, having failed the blanket version of the check on its own.

## Stage 3: machine code, and why it was not yet faster

`make jit` compiles fully-typed numeric functions to arm64 and runs them. It
worked, it was verified, and for three rounds **it was not faster than the
interpreter.** That is worth keeping, because what finally moved it was not any
of the things the failures pointed at.

### What it does

A function whose every arithmetic operand is proved numeric is compiled to real
instructions: `ldr d0` / `fmul d0, d0, d1` / `str d0`, with no type test, no
guard and nothing to deoptimise to. NaN-boxing is what makes the load free — a
number's `Value` *is* its double, bit for bit, so a slot goes straight into a
floating-point register. That is also a hard dependency rather than an
optimisation: under `make tagged` a `Value` is a 16-byte struct with a separate
tag, so the backend refuses outright rather than emitting something wrong.

On Apple Silicon the pages need `MAP_JIT`, `pthread_jit_write_protect_np`
around the writes, and `sys_icache_invalidate` afterwards. Without the last one
the processor executes whatever was in the page before.

Correctness is checked the same way the IR was: `make test-ir` runs the
compiled code in place of the interpreter and requires all 79 golden cases to
be unchanged.

### The measurement, at the end of stage 3

| Benchmark | Interpreted | Compiled | Node |
| --- | ---: | ---: | ---: |
| 3M calls to a two-multiply function | 138 ms | **131 ms** (−5%) | 6 ms |
| one call, 20M-iteration loop | 473 ms | 484 ms (+2%) | 34 ms |

Three attempts to get there, and the shape of them is the useful part.

**Naive: 26% slower.** Every IR value went to memory and came back, so a
two-multiply function did more loads than the bytecode did.

**Forwarding redundant slot round-trips: parity.** A load whose slot has not
been written since the store that fed it reads a value already in hand.

**A per-block linear-scan register allocator: −5% on calls, +2% on loops.**
Values get `d2`–`d7` and `d16`–`d31`; `d8`–`d15` are skipped because they are
callee-saved and using them would need a prologue.

One measurement inside that one is worth keeping. Giving comparison results a
floating-point home made the loop benchmark **15% worse**, because a boolean is
a NaN-boxed singleton rather than a double: it has to cross to the general
register file to be produced and back again to be tested. Two moves to save one
store. Booleans stay in memory, and that single change is the difference
between +15% and +2%.

### The diagnosis that was wrong

The conclusion drawn at the time was that the allocator was per-block, and that
a loop body spans several blocks, so loop-carried locals kept a memory home and
the loop could not move until liveness was computed over the whole graph.

That reasoning was sound and the premise was false. It is recorded here because
it survived three rounds of optimisation aimed squarely at it.

## Stage 4: the loop, and what was actually wrong with it

| Benchmark | Interpreted | Compiled | Speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/jit_calls.cx` — 3M calls | 136 ms | 112 ms | 1.2× | 8 ms |
| `bench/jit_loop.cx` — one call, 20M iterations | 492 ms | **55 ms** | **8.9×** | 35 ms |

`make bench-jit` reproduces this. It times the *same* `jit` binary twice, once
with `CS_JIT_THRESHOLD` raised out of reach, so the difference is the compiler
and not the build configuration — the back-edge counter is present in both
columns.

### Four optimisations, and the one that mattered

**Slot promotion.** A slot every store to which is a proved number can live in
a register for the whole function, loaded once on entry. Six of `work`'s eight
slots qualify. This was the change predicted to fix the loop. On its own it
moved it by 0%.

**Dead-store elimination.** A slot that appears in no load anywhere is
write-only, so every store to it is dead. Whole-function and trivially safe,
and it took the loop body from 19 IR instructions to 11. Calls: −17%. Loop: +1%.

**Constant hoisting.** A 64-bit immediate takes up to four `movz`/`movk` and an
`fmov` to reach a floating-point register, and the loop body was paying that
three times per iteration for values that by definition never change. They now
get registers alongside the promoted slots and are materialised once on entry.

**Compare-and-branch fusion.** Only possible once dead-store elimination had
made the comparison adjacent to its branch; before that the lowering put a
store between them. The condition stays in the flags instead of becoming a
`Value`.

After all four: calls −18%, loop **−2%**. The loop body was by then eleven
instructions, entirely in registers, with a fused branch. There was nothing
left in it to remove.

### The compiled code was never running

```
$ CS_JIT_REPORT=1 ./build/jit/cscript bench/jit_loop.cx
  1 of 1 compiled to machine code
  0 calls answered without the interpreter
```

`work` is called **once**. It crosses the threshold at a loop back-edge —
which is the case the back-edge counter was added to catch — and by that point
the only call to it is already in progress. Compiling for the call site is
compiling for a call that never comes again. Every loop measurement in stage 3,
and every one of the four optimisations above, was the interpreter timed
against itself.

The counter existed. What was missing was any way to use what it found.

### On-stack replacement

Entering compiled code mid-loop normally means reconstructing the machine
state a compiler assumed from the state an interpreter actually has. Here it
costs almost nothing, because of a property the design already had: **the
compiled code keeps locals in the interpreter's own frame, at the interpreter's
own offsets.** There is no layout to translate. The frame pointer is passed
straight through.

So each loop header gets its own entry point — the function prologue again
(load the promoted slots, materialise the constants) followed by a jump into
the header — and `OP_LOOP`, having counted the back-edge, checks for one:

```c
if (frame->closure->function->jitState == JIT_COMPILED) {
  Value produced;
  if (csJitOsr(function, ip - chunk.code, frame->slots, &produced)) {
    /* the compiled code ran the function to completion */
  }
}
```

The prologue is the whole of the state transfer. Promotion means a slot's live
value belongs in a register; at hand-over the live value is the interpreter's,
in the frame; reading them is what makes the two agree.

**What makes it safe** is checked rather than assumed. Registers in this IR are
operand-stack positions, so a register live across a block boundary means the
operand stack was not empty there — and the interpreter's stack and the
compiled code's scratch array are different memory. `blocksAreSelfContained`
requires that no block reads a register it did not itself write. Where that
holds, all live state is in slots, both sides agree on where those are, and the
hand-over is a jump. Where it does not, the function simply gets no OSR entry.

Result: 492 ms to 55 ms, and within 1.6× of V8 on the same program.

### What this says about the earlier failures

The four optimisations were not wasted — with OSR in place they are what makes
the loop body eleven instructions instead of thirty. But they were measured
against a benchmark that could not see them, and each null result was read as
evidence about register allocation when it was evidence about entry points.

The general lesson is narrower than "measure everything", which was already the
rule here. It is that a benchmark can agree with the answer, agree with Node,
run the right binary, and still be measuring nothing at all. `csJitDumpProfile`
now prints loops taken over as well as calls answered, because a gate that
never opens proves nothing and the only defence is to count.

### What is still slow

`jit_calls` gains 1.2×, not 8.9×. `dist` is two multiplies and an add, and
around it sits the whole cost of getting into compiled code: a linear scan of
the hot table, an argument copy into a slots array, a call and a return. For a
function that small the entry overhead is most of the work. Inlining is the
answer to that one, and it is not written.

## Stage 5: taking part of a function

| Benchmark | Interpreted | Compiled | Speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/locals.cx` | 149 ms | **18 ms** | **8.3×** | 7 ms |
| `bench/loop_empty.cx` | 170 ms | **20 ms** | **8.6×** | 7 ms |
| `bench/globals.cx` | 211 ms | **41 ms** | **5.2×** | 7 ms |

None of these has a type annotation in it. Their types come from the checker's
inference of `let a = 0` — which is the whole argument for inference made
concrete: annotate nothing, and the compiler still knows.

### Whole functions were the wrong unit

Every benchmark in the suite was refused, and all for the same reason. A script
ends in `console.log`, `OP_INVOKE` has no IR form, and the lowering refused the
function — including the numeric loop above the call, which it understood
perfectly well.

`IR_EXIT` hands the frame back to the interpreter at a bytecode offset. It is
the mirror of the hand-over that already existed: on-stack replacement works
because the compiled code keeps locals in the interpreter's own frame at the
interpreter's own offsets, and leaving works for the same reason. It costs a
write-back of the promoted slots and nothing else.

**Where an exit may go** is the whole of the difficulty. A value pushed since
the block started lives in a register the interpreter has no name for, and
writing it back would mean knowing which register — which the allocator is free
to have reused for something else. So an exit only goes where the operand stack
is at its block's *floor*: the lowest it has been since the block began.
Everything the block has written is at or above that mark, because to write
position `p` the stack has to have been `p` deep, so everything below it is
still exactly where the interpreter left it.

The thing that forces an exit — a call, a string concatenation, arithmetic on
something unproved — is normally found with its operands already pushed. So the
exit *rewinds* to the last point the stack was at the floor and drops every
instruction emitted since. None of them ran; the interpreter redoes that
statement from its beginning.

### Globals, and what makes an address safe to bake

A module binding inside a compiled loop is one load. Not a hash, not a version
compare — the address of the table entry is baked into the code and kept in a
register for the length of the run.

That is only safe because of something the rest of the design already
guaranteed: nothing inside a compiled region can call anything, so nothing can
add a binding and force the table to rehash while the code runs. Between runs
it can, so the table's version is checked on the way in.

The type comes from asking. The lowering runs at the moment a function turns
hot, which is the one moment it has a running program to ask what a binding
holds; a global holding anything but a number hands the frame back instead.
What keeps the answer true afterwards is the language rather than a guard: the
checker will not let a declared binding change type. The entry check that it is
still a number is for the case the checker does not cover, and costs one test
per entry rather than one per access.

A *definition* lowers as a store. By the time a function is hot its module has
already run, so `let n = 0` at the top of a script is a store to a binding that
exists — which matters because that single instruction used to stop everything
below it from lowering at all.

### Calling out, and what it is worth

`%` is the one thing here that cannot be an instruction: arm64 has no
floating-point remainder, and the obvious inline form `a - b * trunc(a / b)`
stops being exact once the quotient passes 2^53. A silently wrong `%` is worse
than a slow one, so it calls `fmod`.

Calling from compiled code normally means spilling every live value, because a
call may clobber any caller-saved register. This avoids all of it by choosing
where values live: **a function that calls anything allocates only from
d8–d15**, the bank a call is obliged to preserve. Nothing live is in danger, so
nothing is spilled. It costs fourteen of the twenty-two registers, and a loop
that fits in eight is most of them.

The three arguments and the baked global addresses moved to x19–x27 for the
same reason, which is what the prologue is for.

| Benchmark | Interpreted | Compiled | Speedup |
| --- | ---: | ---: | ---: |
| `bench/loop_arith.cx` — `sum += i % 7` | 296 ms | 186 ms | 1.6× |
| `bench/branches.cx` — two `%` per iteration | 232 ms | 161 ms | 1.4× |

Well short of the 5–9× everything else gets, and the reason is the call itself
rather than anything around it: hoisting the `fmod` address out of the loop —
four instructions an iteration — moved 1.5× to 1.6×. Ten million libm calls
cost what ten million libm calls cost.

Which is what made the next change obvious. **A loop counter's remainder is an
integer remainder**, and the interpreter had taken that path since long before
there was a compiler — so compiled code was slower at `%` than the code it
replaced. It now emits `sdiv`/`msub` inline where both operands round-trip
through the integer registers unchanged, and calls `fmod` only where they do
not. Exactness decides, not a guess: zero on either side goes to `fmod` too, so
that `0 % n` keeps its sign and `n % 0` still gets its NaN. Both benchmarks
moved past 10×, and they are no longer the two slowest things in the table.

### Six soundness bugs, and the harness that found them

The first three were older than side exits. Side exits made them reachable.

**Slot types were tracked in linear order.** A slot read where it happened to
hold a number was typed `number` even when another store put something else
there — and a loop back-edge makes "another store" mean "the previous
iteration". A load typed `number` was reading a boolean, with arithmetic
compiled around it and no guard to check. `csIrReconcileSlotTypes` now makes
each slot's type the meet of everything stored into it and downgrades the loads
that claimed more, iterated to a fixed point because downgrading a load
downgrades whatever is computed from it.

**Dead-store elimination removed stores whose only reader is the interpreter.**
After an exit the interpreter's next instruction may load any live slot — a
read that is not in the IR at all. A loop computed the right answer and handed
back the value it started with. An exit now counts as a use of every live slot.

**Dead code after a terminator poisoned the types around it.** The compiler
appends an unreachable `return undefined` to every function, and the store
setting up its value made the slot it used look as though it held two different
types — so the slot went to `unknown`, the loads of it went to `unknown`, and
the loop that used it stopped compiling. It is cut now, before any pass reads a
type. This one arrived *with* the reconciliation that fixed the first bug, and
was not noticed for two commits, because the differential harness proves the
two paths agree and says nothing about how much the compiler took. It now
reports how many programs it took part in, where losing one is visible.

**The branch condition of a short-circuit came from the wrong path.**
`a || b || c` compiles to a chain of jump-if-true, and the jump ending each arm
lands on the next one — so each of those instructions *begins a block with two
predecessors*. The lowering read the condition out of its own model of the
operand stack, which holds the register the **linear** walk last pushed there;
on the path that arrived by jumping, that register was produced by an
instruction which never ran. The answer was wrong rather than absent: a
four-term chain after an early return answered false for its second term, which
is how `std:encoding` came to escape a `.` that RFC 3986 says to leave alone.

The condition is now loaded from the slot, which is the one thing both paths
agree on, and the abstract stack is forgotten at every block boundary so that
no other site can make the same assumption — the five places that read it all
refuse a forgotten entry rather than guessing, so the cost is a function left
uncompiled rather than one compiled wrongly. Coverage did not move: 43 of 217
programs before and after. Found by `make test-ir` on a library module, not by
a test written for the compiler, which is the argument for running the whole
suite through the IR rather than a suite of its own.

**Three passes each mis-read an instruction's operands.** The same two fields,
`a` and `b`, hold a virtual register, a slot number, a block index, a
constant-pool index, a bytecode offset or an operand-stack height depending on
the opcode. The register allocator marked constants as escaping; slot
forwarding renamed an exit's *bytecode offset* into a register number; the
self-containment check read a stack height as a value. `csIrRegisterOperands`
answers it in one place now and all three ask it.

**`===` on anything but a number was false.** The fourth, found by reading
rather than by running, and the only one the differential harness had never had
a case for. Every comparison the encoder emits is `fcmp`, which is right for
the ordered ones because `csIrIsFullyTyped` refuses a function whose `<` has an
operand nothing proved numeric. Equality is deliberately outside that check —
`===` is defined for every type, and the IR interpreter answers it with
`csValuesStrictEqual`, which is right for every type. `fcmp` is not: a Value
that is not a number is NaN-boxed, which is to say it *is* a quiet NaN, so
comparing two of them is unordered and the equal branch is never taken. `s ===
s` on a string returned false, once the loop around it got hot.

The encoder refuses it now rather than the lowering, which keeps the IR
interpreter's coverage: it can run these correctly and only the machine code
cannot. A bitwise compare would not have been the fix either — it gets `NaN ===
NaN` and `-0 === 0` wrong in the other direction, which is what
`tests/cases/jit/jit_equality.cx` pins alongside the original case.

**A branch read its condition out of memory the value was never in.** The
fifth, and the only one of them that was giving a wrong answer to ordinary
code. A comparison's result is deliberately kept in memory — the allocator
refuses it a register, because the branch that reads it reads the scratch
array — and every branch there was came from a comparison, so the encoder
loaded from scratch unconditionally. `while (true)` branches on a *constant*,
and a constant does get a register, so nothing ever wrote the slot the branch
was reading:

```js
function countUp(n: number): number {
  let total: number = 0;
  while (true) { total = total + 1; if (total > n) break; }
  return total;
}
countUp(200000);   // 200001 interpreted, 99 once the loop got hot
```

The branch now reads the register when the value is in one. The differential
harness had no case with a hot `while (true)` in a fully typed function, which
is why two years of runs never asked the question — `tests/cases/jit/`
`jit_conditions.cx` asks it now, in all four shapes that produce a constant
condition.

None of this was caught by the golden files, and it could not have been. A
`.expected` file pins what a program prints; it says nothing about whether the
compiled path and the interpreted path agree, and that is precisely where a
side exit goes wrong.

`make test-jit` runs every program in the tree **twice in the same binary** —
with the tiering threshold raised out of reach, and with it at one — and
requires the two to agree byte for byte. It found the first of these on its
first run. It also counts what the compiler actually took, because a sweep of
agreements over programs the compiler declined is not coverage:

```
checked 121 programs, 0 disagreed
the compiler answered 3000549 calls, loops and exits across them
```

That counter exists because of the lesson in *Stage 4*: a benchmark that
agreed with Node, ran the right binary and measured nothing at all. A gate that
never opens proves nothing, so everything here counts how often it opened.

## Stage 6: inlining, a call that is not there

| Benchmark | Interpreted | Compiled | Speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/jit/jit_inline.cx` — a call inside a 20M-iteration loop | 817 ms | **49 ms** | **16.7×** | 34 ms |
| `bench/jit/jit_loop.cx` — the same loop, without the call | 434 ms | **49 ms** | **8.9×** | 35 ms |

Those two programs are the same loop. One of them calls a three-operation
function to compute what the other writes inline, and interpreted that costs
1.9× — which is the ordinary price of a call. Compiled, they finish within a
millisecond of each other, because after lowering there is no call in either.

### Why a call was the wall

A call is the one thing this backend cannot emit, and not for want of an
instruction. Compiled code keeps its locals in the frame the interpreter gave
it and holds everything else in registers; it opens no frame of its own, and
there is no point in it at which the collector could walk one. Calling out
means building both — a frame the callee can return into, and a safepoint where
the values live in registers can be found.

So a call in a loop body was not slow. It was a hand-over: the lowering reached
`OP_CALL`, could not express it, and gave the frame back at the last point the
operand stack was at the block floor. The loop above it went with it.

### What a spliced body needs instead

Nothing, which is the point. A callee qualifies when its body is straight-line
arithmetic over its own parameters — no branches, no calls of its own, nothing
that can throw past the caller, no `this`, no captured local, no default or
rest parameter. For a body like that the frame the VM would have built is
*unobservable*: nothing can read it, nothing can unwind through it, and nothing
outside can see it exist.

An unobservable frame does not have to exist. The callee's slots become a
compile-time map onto registers the caller already holds — position k + 1 is
the register holding argument k, and a local the body declares is whatever
register the expression that declared it produced. `const scaled = a * 3 + b *
7` opens no slot at all; it names a register. What comes out the other side is
arithmetic the register allocator cannot tell from code that was written
inline.

### Finding the callee, and holding on to it

The callee is read out of a module binding, so it is known at lowering time —
the function only reached the compiler by being hot, which is after its module
ran. But a binding is not a constant. `step = somethingElse` between the
compile and the run would leave a body inlined for a function nobody is
calling, so the binding is checked to still hold the same closure before either
entry runs, exactly as the property shapes are. That check is also what keeps
the closure alive: rebind the name and the check would be the only reference
left, and a freed closure whose memory came back as a different one would make
it pass exactly when it must fail.

### The placeholder, and why it is safe

Splicing happens at `OP_CALL`, but the callee is pushed several instructions
earlier by an `OP_GET_GLOBAL`, and the arguments go on top of it. The lowering
mirrors the operand stack into frame slots, so pushing the closure as an
ordinary value would store an object into a slot the loop also uses for
numbers — and one slot may hold one type, so the loop would stop compiling for
the sake of a value the compiled code never reads.

Instead the position holds nothing at all: no register, no store, no load.
Which is only sound because the call that takes it off is found *first* — the
lowering walks forward from the callee load, modelling the stack depth of the
argument expressions, and refuses unless it reaches an `OP_CALL` with exactly
that many values on top, in the same straight run, with no jump landing in
between. A position holding no value has to be taken off by something, and the
walk is the proof that the something exists. Where it does not, the callee load
hands the frame back, which is what it did before inlining existed.

### What it does not take

Anything with a branch in it, which is most real functions; anything that calls
something else; anything reading a property or a global, because those carry
indices into the *callee's* constant pool and inline caches rather than the
caller's. The bound is 32 bytecode instructions per callee and 128 per caller,
both arbitrary, because inlining trades code size for calls removed and without
a limit a chain of small functions is one large one.

The rest still wants the frames and safepoints. Inlining does not replace that
work — it takes the callees for which the work is unnecessary, which turns out
to be the ones a hot loop is full of.

## Stage 7: what the interpreter does in between

| Benchmark | Interpreted | Compiled | Speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/jit/jit_calls.cx` — 3M calls below a function declaration | 132 ms | **6 ms** | **23.7×** | 6 ms |
| `bench/calls.cx` — an unannotated helper in a 2M-iteration loop | 115 ms | **5 ms** | **24.0×** | 3 ms |

Both were 1.4× before, and the compiler did not change. What changed is that
the lowering stopped throwing away everything below its first hand-over.

### Why a hand-over ended the lowering

Side exits let the compiler take part of a function: it gives the frame back at
the last point the operand stack was at the block floor, and the interpreter
carries on from there. What it could not do was *resume*. Everything after an
exit runs in the interpreter, so the operand-stack height on the far side was
unknown — and in this VM a local is a stack slot, so a block whose entry height
is unknown cannot be lowered at all. The wrong height means the wrong slot,
with no symptom until the answer is wrong.

The lowering therefore set `skipped` and, from then on, would only lower a
block whose height some already-lowered jump had recorded. After a hand-over at
offset zero there are none, so nothing was lowered at all.

Offset zero is not a corner case. It is where a script declares a function:

```js
function dist(x, y) { return x * x + y * y; }   // OP_CLOSURE — handed back here
let total = 0;
for (let i = 0; i < 3000000; i++) total = total + dist(i, 2);
```

`OP_CLOSURE` is the first instruction, the lowering has no IR for it, and the
loop below it went with it. Both call benchmarks are that shape, which is why
both sat at 1.4× while everything else reached 5–9×: they were measuring the
cost of entering compiled code for `dist`, not a compiled loop.

### The height was never unknown

Nothing had worked it out. Where every instruction in the skipped run is one
whose effect on the frame is fixed, the height at the far end follows from the
bytecode — and so does what each slot holds, which matters just as much: the
loop counter in that example is established by an `OP_CONSTANT` inside the
skipped run, and without its type the comparison at the loop header is refused.

It is the same bytecode the interpreter is about to run, so the two cannot
disagree. That is what makes it a proof rather than a speculation, and why it
needs no entry check to go with it.

The replay starts at the *exit's* offset, not at the instruction that forced
one — the hand-over rewinds to the block floor and the interpreter resumes
there, so that is where the run being modelled begins. It covers constants,
closures, locals and globals, the arithmetic that throws rather than coerce
(`-` leaves a number or does not return; `+` may leave a string, so nothing is
claimed), `++` and `--` on a local (which refuse a slot that is not a number,
so reaching the next instruction proves one is there), and a completed call —
which leaves one value where the callee and its arguments were, whatever it did
in between.

What it did in between is the interpreter's business. Every assumption the
compiled code holds — the global table's version, the object shapes, the
inlined callees — is checked on the way in, and the way in comes after all of
this has run. Anything the model does not cover gives up exactly as before, and
the slot types are written back only when the whole run is modelled, so a run
abandoned halfway leaves the lowering's view of the frame untouched.

One consequence needed guarding. A position the interpreter filled holds no
register this function produced, so the lowering's abstract stack is reset to
"no register here" across the replayed run, and the places that read a register
straight off that stack — the numeric-operand check, the property paths — now
refuse a position that has none instead of indexing the register table with
`-1`.

### The jump at the end of the run

A run handed over to the interpreter can end in a jump, and usually does: the
scan that finds where the run stops looks for the next *leader*, and a jump
makes the instruction after it one. So there is at most one, it is last, and
modelling its fall-through is only half the answer.

The other half is where the taken arm goes. That target acquires a predecessor
the IR does not know about, and every entry type derived for it then comes from
the wrong set of paths — which is why the replay refused a jump at first, and
why one `if (w > 3) break;` inside a run handed over for a `console.log` cost
`loops_control.cx` its whole lowering.

The arm is recorded instead, as one more incoming path: its height, and what
each slot holds when it is taken. The height is the part a meet cannot fix —
it decides which slot every emitted instruction names — so a disagreement there
is reported rather than reconciled, and the function falls back. Both arms of a
fused compare-and-branch consume their two operands, so in practice they arrive
at the same depth; a `break` pops the locals of the scope it leaves before it
goes, which the replay has already modelled by the time it reaches the jump.

An unconditional jump, and a return, are the cases with no second arm to model:
the end of the run is never reached, so the height there is not a fact about
anything. The walk says so and picks up again wherever a predecessor recorded a
state.

**Every lowered jump records its types too, for the same reason.** A block the
walk resumes at after a skip needs to get its slot types from somewhere, and
its own carried-forward model is exactly the thing that cannot be believed. So
`reachBlock` merges the state at the jump into its target alongside the height
it already recorded — which is what let the loop below a `continue` keep its
counter's type instead of losing the function to it.

### What it cost, which is the interesting part

Every step of this exposed something older than itself, and none of it was
reachable while a hand-over ended the lowering.

**Slot types were one per function.** `csIrReconcileSlotTypes` took the meet of
everything stored into a slot, because the IR is deliberately not SSA — and the
operand stack and the locals are the same array, so different loops in one file
reuse the same positions. A `while (true)` putting a boolean where a counter
had been gave that position no type at all. That is
[Stage 8](#stage-8-one-type-per-slot-was-one-too-few).

**The reachability walk did not follow fall-through.** A block the lowering
cuts short of a terminator runs into the next one, and the walk that proves no
compiled path reaches a block with a *fabricated* entry height only followed
jumps and branches. So such a block could be declared unreachable and then be
reached, handing the interpreter a frame at the wrong depth — `labels.cx` did
it, and the answer was a boolean where a loop counter should be.

**A block resumed after a skip carried a stale belief about its slots.** The
height came from a jump and was a fact; the types were left over from the path
the lowering had abandoned. Only advisory — the dataflow retypes every load
afterwards and refuses a function whose arithmetic loses its proof — but the
wrong thing to record as a path that happened. Such a block now takes its types
from a predecessor, or claims nothing.

## Stage 8: one type per slot was one too few

The IR is deliberately not SSA — locals stay in numbered slots and only
expression temporaries become virtual registers — and the price of that was a
single type per slot for the whole function. `csIrReconcileSlotTypes` took the
meet of everything stored into a slot anywhere, which is sound and is also
wrong in a way that costs, because in this VM the operand stack and the locals
are the same array:

```js
function twoLoops(n: number): number {
  let total: number = 0;
  for (let i: number = 0; i < n; i++) total = total + i;   // i in position 3
  while (true) {                                           // `true` in position 3
    total = total + 1;
    if (total > n) break;
  }
  return total;
}
```

`i` and the `while`'s condition occupy the same frame position, one after the
other. The meet of a number and a boolean is nothing at all, so the comparison
at the top of the first loop had an operand nothing could prove numeric — and
`csIrIsFullyTyped` is a whole-function verdict, so the *entire* function was
refused for it.

### The dataflow, and the one edge the jumps do not name

The precise answer is a meet over each block's predecessors: a block's entry
types are the meet of its predecessors' exit types, iterated to a fixed point.
That needs no SSA and no dominance, only the predecessors — which the jumps
already name, plus the fall-through into the next block, which the code
generator relies on when it lays them out in order.

And one more the jumps do not name: **the lowering's own walk in bytecode
order.** That is a real path into a block — the one the interpreter takes to
get there — and it is the only one on the way into a loop the compiler takes
over part-way through, where no lowered jump reaches the header at all. So the
lowering records what it believed each slot held on the way into each block,
and the dataflow meets that in as one more incoming edge.

The lattice is two deep, so a handful of sweeps settles it. The whole-function
meet is still computed, because the register allocator's question — can this
slot live in a register for the whole run? — is a whole-function question
however precisely the loads are typed.

### When the walk cannot be believed

Where the lowering skipped a run it could not replay, its linear state
describes a path that did not happen, and from that point nothing recorded can
be trusted. The whole function falls back to the coarse meet, which is what
this pass did before there was a finer one — so the precision is an addition
rather than a replacement.

A conditional jump inside a skipped run is what triggers that, and not because
its fall-through is hard to model. It is where the *taken* arm goes: the target
acquires a predecessor the IR does not know about, and every entry type derived
for it comes from the wrong set of paths. `tests/cases/language/`
`loops_control.cx` has exactly one, and is the only program in the tree the
compiler no longer takes part in. Merging the replay's state at the jump into
the target's entry state is the fix, and it needs the *height* to agree as well
as the types — a `break` pops the locals of the scope it leaves, so the two
arms of one jump do not always arrive at the same depth.

### What it found

Making `while (true)` compilable is what turned up the fifth soundness bug
above. That is the pattern this project keeps running into and the reason the
differential harness counts coverage rather than only agreements: a check that
never runs on a shape proves nothing about that shape, and the way to find out
is to make the shape reachable.

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
| `make switch` | `-O2` + portable switch dispatch | `make test-switch` |
| `make tagged` | `-O2` + 16-byte tagged `Value` | `make test-tagged` |
| `make profile` | `-O2` + opcode and opcode-pair counters | Finding what to fuse |
| `make jit` | `-O2` + tiering, the typed IR and the backend | `make test-ir`, `make test-jit`, `make bench-jit` |

UBSan is in the default debug build because it is portable and cheap, and
`-fno-sanitize-recover` makes undefined behaviour abort rather than warn.
AddressSanitizer is a separate target because it does not always start: on some
macOS builds its runtime deadlocks inside `AsanInitInternal`, spinning on its
own mutex before `main` is reached, and under some sandboxes it fails outright.
Neither should be able to wedge `make test`.

## Stage 9: the store that adds

A store that *overwrites* has been compiled since stage 5, and a store that
**adds** was what excluded almost every constructor: `this.x = x` transitions
the object's layout, so the two stores in a two-field constructor expect two
different shapes, and one slot may only carry one.

An add is two stores — the value, then the new layout — provided three things
are known. The first two come from the site's inline cache, which now records
the *pair* it saw rather than only the result: the layout on the way in, and
the one the object takes on. The third is that the storage already has room,
because growing it allocates and compiled code cannot allocate.

So the cache holds a transition, the lowering follows the chain of them
through a block — each add's expected layout is what the last one produced —
and the entry check gains one question beside the shapes: has this object room
for every property the body will add?

Room is what an instance is now built with. It reserves four slots, which is
what the storage grows to on its first property anyway: the allocation is
moved from the middle of the constructor to the moment before it, where it is
not in anybody's way. An instance that gains no property at all pays the 32
bytes it would have spent on gaining one.

The same cache pair makes the *interpreter* faster, and that is most of the
gain: an add there was a shape lookup, a transition lookup and a possible
grow, and is now a compare and two stores.

| Benchmark | Before | After |
| --- | ---: | ---: |
| 3M constructions, compiled | 0.45 s | **0.31 s** |
| `bench/classes.cx`, compiled | 0.25 s | **0.18 s** |
| 2M constructions, interpreted | 0.29 s | **0.22 s** |

What is still refused: an object whose storage is full, a site that has seen
more than one layout, and a constructor whose stores are not all adds of known
names — `this[key] = v` names nothing at compile time.

## Stage 10: allocating in compiled code

Compiled code could not allocate, and so could not build an object — which is
what the loop in `bench/properties` does three million times. What stood in the
way was not the allocation but the collector: it walks the interpreter's stack
up to `stackTop`, and neither way into compiled code puts its frame there. A
call entry builds the slots as a local array; a back-edge hands over the
interpreter's frame, but compiled code writes past what the interpreter has
opened.

A **root range** answers both, and a range is enough rather than a stack map
because every value compiled code holds is a number except the objects in
frame slots. The VM is told where the frame is for exactly as long as a run
lasts. The IR interpreter gets a second range for its register file, which can
hold an object between a load and its use.

Then the rule that makes the build itself safe: **the object goes into its
frame slot before any property is put on it.** From that instant it is inside
the range, and putting a property on it may allocate. The values come from the
slots above the destination, written there by ordinary stores — so they are in
the frame rather than in registers, and the emitted code is a call with three
arguments instead of a list.

What it cost: `bench/properties` went from 0.32 s to **0.20 s**, and the
differential suite's coverage rose from 42 programs to 46, with 73 reaching
machine code where 70 did.

### Two things the first attempt had missed

It was withdrawn once, for a wrong answer on this same benchmark that was never
diagnosed. Neither cause was among the three it had already fixed.

**A fourth pass reasoned about slots by looking for stores.** The one that
turns a store followed by a load of the same slot into a register rename. A
slot written by an allocation looked untouched to it, so a load *after* the
allocation was renamed to the register holding what the slot held *before* —
the previous iteration's object, read back with nothing to see: no crash, no
refusal, no diagnostic. Every pass now asks `csIrWritesSlot` and
`csIrReadsSlots` rather than looking for `IR_STORE_LOCAL` itself.

**The emitter's switch had no `default`.** An instruction it did not know
emitted *nothing* — so the allocation silently did not happen and the slot was
read as an object anyway. That is how the first version of this change
segfaulted instead of refusing the function. It refuses now, which gives up the
`-Wswitch` warning that would have named a new opcode and buys a backend that
cannot quietly skip one.

## Stage 11: calling a function from compiled code

Inlining (stage 6) answers a call by not making one, and takes the callees for
which that works: small, straight-line, no branches. Everything else handed the
whole of the caller back to the interpreter — `digitSum` in
`bench/jit/jit_calls_out` is four lines long, has a loop, and cost the outer
loop every one of its three million iterations.

**The frame is the operand stack, so a call needs nothing new.** The arguments
go into the slots above the callee's own position, which is where the
interpreter would have left them; the result comes back into that position,
which is where the interpreter leaves one. The emitted code is a call to one C
helper with the frame, the destination and a call-site index — the same shape
as an object literal's, and for the same reason: the values are already in the
frame, so there is no list to build.

**Both frames have to be rooted at once.** `VM.jitRoots` was a single range,
replaced for the length of a run. A compiled function that calls a compiled one
has two frames live, and replacing would have unrooted the caller — so it is a
stack of ranges now, pushed on the way in and popped on the way out. Nothing in
`markRoots` changed but the loop around it.

**A callee that throws ends the compiled frame.** The exits stage 5 introduced
hand a frame back at a bytecode offset, which works because nothing before that
point has run twice. A failed call is not like that: the callee already ran, so
resuming the caller anywhere would run it again. The exit records
`CS_JIT_EXIT_FAILED` instead of an offset, and both entry points —
`csJitTryRun` and `csJitOsr` — report it as `failed` rather than as a frame
handed back. The interpreter takes the throw where it stands, through the same
`HANDLE_FAILED_CALL` a native callback has always used.

**Arithmetic on the result needs the callee's declared return type.** A call
whose result is an unknown value is a value the compiled code may move but not
add to, and `total = total + digitSum(i)` is an add. `ObjFunction` carries the
return type the checker proved, so a callee that said `: number` lets the
caller's arithmetic compile. Without it the call lowers and the function is
then refused for the addition, which is the shape of a feature that technically
works.

Nothing had to be spilled around the call: a function that calls out already
allocates only from `d8`–`d15`, the registers the C ABI obliges a callee to
preserve. That was built for `IR_MOD`, which calls `fmod`.

### Two silent failures on the way

**The splice's rollback took the arguments with it.** `OP_CALL` tries inlining
first and drops what the attempt emitted when it does not take. The mark it
rolled back to was taken *before* the arguments came off the abstract stack —
and taking an argument off may emit a load. So the fallback stored registers
whose defining instructions had just been deleted, and the callee was handed
whatever those registers happened to hold. `digitSum` was called with garbage
and answered 0: a plausible number, no crash, no refusal. The mark is taken
after the arguments now.

**The call was slower than not making it.** `csVMCallCallback` — what a native
uses to call back into user code — goes straight to `callClosure`, skipping the
entry that consults the compiler, so a compiled callee reached that way is
*interpreted*. The first working version ran `jit_calls_out` in 346 ms against
the 293 ms it took with the caller handed back and only the callee compiled:
the feature was a regression, and passing tests said nothing about it.
`csVMCallFromCompiled` goes through `callValue` instead — the ordinary entry,
so a compiled callee is entered as compiled code and a native answers directly.

| `bench/jit/jit_calls_out` | Interpreted | Compiled |
| --- | ---: | ---: |
| Before | 831 ms | 293 ms |
| Through `csVMCallCallback` | 850 ms | 346 ms |
| Through `callValue` | 850 ms | **170 ms** |

One thing is knowingly given up: a compiled frame is not on `vm.frames`, so a
stack trace taken inside a callee does not name the compiled caller that made
the call.

## Stage 12: deoptimisation

An exit hands the frame back at a bytecode offset and the interpreter carries
on from there. It has existed since stage 5. Two restrictions on it meant that
most of what it was for did not happen, and both were in the machinery rather
than in the idea.

### A body with an exit could not be entered by a call

On a back-edge entry the interpreter's own frame is what compiled code runs on,
so handing it back costs nothing: put the instruction pointer at the offset,
put the operand stack at the height, carry on. A call entry has no frame at
all — `csJitTryRun` builds the slots as a local array belonging to the run — so
there was nothing to hand back, and any function with an exit anywhere in it
was refused the call entry outright. One unsupported opcode in the tail of a
body meant every call to it was interpreted from its first instruction,
including a loop the compiler would otherwise have taken. 40 of the 261
functions the test corpus lowers were in that position.

`csVMDeoptimise` builds the frame on the way out. It pushes the frame the call
would have got, copies the compiled slots into it, points the instruction
pointer at the exit's offset and sets the operand stack to the exit's height.
The frame stays announced to the collector until the copy is done, because
pushing a frame may allocate and until then that array is the only thing
holding what compiled code built.

Only for a call whose arity is exact. A missing argument, a rest parameter and
a parameter with a default are each filled in by something that runs *before*
the offset being resumed at — `csVMCheckArity` pads, and the defaults prologue
sits at the top of the body — so a call needing any of them would have it done
twice. Exactly as many arguments as parameters, and none of it arises.

Three call sites take a deoptimised frame differently, and the difference is
what each promises its caller. An ordinary call and a method call both report
a call that was *made*, and their dispatch loops recompute the frame pointer
either way. A constructor cannot: `new` decides its result from what the body
answers, so a deoptimised constructor is run to completion in a loop of its
own, exactly as the uncompiled path already was — which is also why
`new.target` has to be set before the compiler is asked rather than after it
declines.

### An exit rewound to the last point the operand stack was empty

The reason given was that a value pushed since then lives in a register the
interpreter has no name for. That turned out not to be true: `csIrPush` emits a
store into the slot as well, because in this VM the operand stack and the frame
are the same array. Every position below the current height is already where
the interpreter reads it from, so an exit can be at the instruction that forced
it, with only that one instruction's own emitted work dropped.

What the rewind cost is easiest to see in a body of straight-line declarations.
`const x = …` leaves its value on the stack and calls that position a local, so
the stack never returns to the block's floor, so the floor never moves off the
block's start. A hand-over anywhere in such a body threw the whole block away —
a function of eight arithmetic statements ending in an array literal lowered to
exactly one instruction:

    ir for mix: 1 block, 105 registers, 13 slots
      block 0:
                      exit     -> bytecode 0, stack 3

The one position that is *not* materialised is a callee placeholder — a global
the lowering resolved to a closure and never pushed a value for. A hand-over
with one live still rewinds to the floor, which is below it by construction.

### What it is worth

| | Compiled | Calls answered by compiled code |
| --- | ---: | ---: |
| `bench/jit/jit_deopt` before | 133 ms | 0 |
| `bench/jit/jit_deopt` after | **61 ms** | 1,990,000 |

The benchmark is a body with one branch the compiler cannot take, reached by
one call in a thousand. Before, that branch cost the other 999 their compiled
code entirely.

Coverage moved further than the clock did: the differential suite went from 52
programs the compiler takes part in to 59, and from 77 reaching machine code to
83. Under a collection at every allocation, from 29–30 and 54 to 38 and 60.

### What this is not

A speculative guard. An exit is still only ever forced by something the
compiler has no encoding for, never by a type it guessed — the entry
assumptions are still checked once, at entry, and a call whose shapes do not
hold is still declined whole. What was missing was the mechanism a guard needs,
and that is what is here: a guard can now fail anywhere in a body, on either
entry, and the interpreter picks the frame up at that instruction. The obvious
first use is the shape a property read was lowered against, checked at the read
rather than at entry, so a site only some paths reach stops refusing the call
the other paths would have been answered by.

## What comes next

The pipeline has not changed shape since the first milestone, which was the
point of building it that way. Everything since has been added inside it: the
parser grew statement forms, the compiler grew scope resolution and classes,
the VM grew opcodes and fibers, the collector grew object types — and one
genuinely new stage, which only runs on code that has earned it.

| Next | What it needs | Why it is next |
| --- | --- | --- |
| Replaying a conditional jump | The taken arm's state merged into its target, height included | It is the one thing still refusing `loops_control.cx` |
| Guards at the site | A shape checked where it is used, with a deoptimising exit behind it | The mechanism is in; what is left is choosing where one check beats one at entry |
| Cross-block liveness | Real dataflow, rather than the block-local approximation the allocator uses | Values crossing a block boundary keep a memory home today |
| An x86-64 backend | A second encoder behind the same IR | The IR and everything above it are already architecture-neutral |

The typing work has its own next step, unrelated to any of this: conditional
types, which `Exclude` and `ReturnType` need.
