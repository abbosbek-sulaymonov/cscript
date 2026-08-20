# CScript architecture

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
| `include/cscript/` | One public header per subsystem |
| **Front end** | |
| `src/lexer.c` | Source text to tokens |
| `src/parser.c` | The token plumbing, the precedence table, and `csParse` |
| `src/parser_expression.c` | Expressions, templates, functions and arrows |
| `src/parser_declaration.c` | Variables, patterns, classes, imports and exports |
| `src/parser_statement.c` | Blocks, conditionals, the loop forms, `switch`, `try` |
| `src/parser_internal.h` | What those four share |
| `src/ast.c` | Node constructors and the arena they live in |
| **Checking** | |
| `src/typecheck.c` | Static checking; annotates the AST with types |
| `src/type.c` | The type lattice and assignability |
| **Back end** | |
| `src/compiler.c` | Emit helpers, scopes, locals, and the node dispatcher |
| `src/compiler_expression.c` | Operators, assignment, `this`/`super`, closures |
| `src/compiler_statement.c` | Control flow and the destructuring a declaration lowers to |
| `src/compiler_class.c` | Classes: members, accessors, statics, constructors |
| `src/compiler_module.c` | Imports and exports, resolved at compile time |
| `src/compiler_internal.h` | The compiler's ambient state and the seams |
| `src/chunk.c` | Bytecode buffer, constant pool, inline-cache arrays |
| **Runtime** | |
| `src/vm.c` | The interpreter loop and everything on its hot path |
| `src/vm_fiber.c` | Suspendable calls, for `await` |
| `src/vm_event.c` | Microtasks, timers, and the loop that drains them |
| `src/vm_internal.h` | The seams between those three |
| `src/object.c` | Heap object types, string interning, promises |
| `src/shape.c` | Hidden classes: the layout an object has |
| `src/memory.c` | The allocator and the collector |
| `src/table.c` | Open-addressing hash table |
| `src/value.c` | Value operations, coercion, number formatting |
| `src/module.c` | Resolving, loading and ordering source files |
| **Standard library** | |
| `src/native.c` | The built-in global environment |
| `src/native_array.c` | Array methods |
| `src/native_string.c` | String methods |
| `src/native_json.c` | `JSON.stringify` and `JSON.parse` |
| `src/native_promise.c` | Promises and timers |
| **Tools** | |
| `src/debug.c` | Disassembler and AST printer |
| `src/diagnostic.c` | Error reporting |
| `src/main.c` | CLI, REPL, file runner |

### How it was split, and what was not

Three files held 41% of the code: `vm.c`, `compiler.c` and `parser.c`. They
were split by **what they handle**, not by phase — the pieces of a
recursive-descent parser are mutually recursive because the grammar is, so
layering them would have been a fiction. Each group shares an internal header
that nothing outside it includes.

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
anything — see [Stage 5](#three-soundness-bugs-and-the-harness-that-found-them).

## Tiering: what a JIT would compile

There is no code generator. There is the thing that has to come before one: a
count of what gets hot, and a verdict on how much of it could be compiled
without guarding every operation.

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

The refusal list in `src/jit.c` is what a *first* backend would leave to the
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

| | interpreted | compiled | Node |
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

| | interpreted | compiled | speedup | Node |
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

| | interpreted | compiled | speedup | Node |
| --- | ---: | ---: | ---: | ---: |
| `bench/locals.cx` | 149 ms | **18 ms** | **8.3×** | 7 ms |
| `bench/loop_empty.cx` | 170 ms | **20 ms** | **8.6×** | 7 ms |
| `bench/globals.cx` | 211 ms | **41 ms** | **5.2×** | 7 ms |

None of these has a type annotation in it. Their types come from the checker's
inference of `let a = 0` — which is the whole argument for gradual typing made
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

### What is still interpreted

Any loop using `%`. arm64 has no floating-point remainder instruction, `fmod`
is a call, and a call needs a frame this backend does not set up. The obvious
inline form, `a - b * trunc(a / b)`, is wrong once the quotient passes 2^53 —
and a silently wrong `%` is worse than a slow one. `bench/loop_arith.cx` and
`bench/branches.cx` are both waiting on this.

### Three soundness bugs, and the harness that found them

All three were older than side exits. Side exits made them reachable.

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

**Three passes each mis-read an instruction's operands.** The same two fields,
`a` and `b`, hold a virtual register, a slot number, a block index, a
constant-pool index, a bytecode offset or an operand-stack height depending on
the opcode. The register allocator marked constants as escaping; slot
forwarding renamed an exit's *bytecode offset* into a register number; the
self-containment check read a stack height as a value. `csIrRegisterOperands`
answers it in one place now and all three ask it.

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
AddressSanitizer is a separate target because it fails to start under some
sandboxed environments, and that should not be able to wedge `make test`.

## What comes next

The pipeline has not changed shape since the first milestone, which was the
point of building it that way. Everything since has been added inside it: the
parser grew statement forms, the compiler grew scope resolution and classes,
the VM grew opcodes and fibers, the collector grew object types — and one
genuinely new stage, which only runs on code that has earned it.

| Next | What it needs | Why it is next |
| --- | --- | --- |
| Calling out of compiled code | A frame, and spilling the live registers around it | `%` and every `Math.*` are waiting on it, and so is anything with a call in its loop |
| Inlining | The above, plus a size heuristic | `jit_calls` gains 1.2× because entry overhead is most of the work for a small function |
| Guards and deoptimisation | A side exit that can also *undo* — the exits here only leave from points where nothing needs undoing | It is what would let the compiler take code the checker has not proved |
| Cross-block liveness | Real dataflow, rather than the block-local approximation the allocator uses | Values crossing a block boundary keep a memory home today |
| An x86-64 backend | A second encoder behind the same IR | The IR and everything above it are already architecture-neutral |

The typing work has its own next step, unrelated to any of this: class names
are not usable as type annotations, and types do not cross a module boundary.
Nominal types are the milestone that fixes both.
