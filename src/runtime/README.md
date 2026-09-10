# `src/runtime/` — the VM

What runs the bytecode: the interpreter loop, the values it moves, the objects
they point at, the collector that reclaims them, the module loader, and the
regex engine.

## The interpreter is one translation unit

`run()` is one function on purpose. Computed-goto dispatch needs the labels and
the cached `ip`/`frame` to live in a single stack frame, and the helpers called
on every instruction have to be in the same translation unit to be inlined.
Splitting it would cost exactly the speed the dispatch is for.

So it is **one translation unit spread over sixteen files**: [`vm.c`](vm.c)
`#include`s the fragments below, and the compiler sees what it always saw.

| Included at file scope, before `run()` | Holds |
| --- | --- |
| [`vm_state.inc`](vm_state.inc) | The one VM, and the stack it runs on |
| [`vm_number.inc`](vm_number.inc) | Errors, and the arithmetic that has to be exact |
| [`vm_property.inc`](vm_property.inc) | Accessors, and asking an object for what it may not have |
| [`vm_call.inc`](vm_call.inc) | Calling, and the arity a call has to satisfy |
| [`vm_invoke.inc`](vm_invoke.inc) | A callee that is not a plain closure |
| [`vm_iterate.inc`](vm_iterate.inc) | Object keys, and what `for...of` pulls from |
| [`vm_upvalue.inc`](vm_upvalue.inc) | Captured locals, the opcode profile, and the dispatch feature test |
| [`vm_throw.inc`](vm_throw.inc) | Where a throw goes |
| [`vm_cache_miss.inc`](vm_cache_miss.inc) | What an inline cache misses |

| Included *inside* `run()` | Holds |
| --- | --- |
| [`vm_ops_value.inc`](vm_ops_value.inc) | Constants, the stack, variables, properties — the most-executed set |
| [`vm_ops_iterate.inc`](vm_ops_iterate.inc) | Indexing, iteration and destructuring |
| [`vm_ops_object.inc`](vm_ops_object.inc) | Spread, private members, deleting a property |
| [`vm_ops_build.inc`](vm_ops_build.inc) | Building an object or an array, and calling |
| [`vm_ops_class.inc`](vm_ops_class.inc) | Classes, `new`, `super`, modules, suspension |
| [`vm_ops_arith.inc`](vm_ops_arith.inc) | Arithmetic, comparison, jumps, control flow |

**Why `#include` and not a call.** Two reasons, in order of force:

1. An opcode body ends in `VM_NEXT()`, which under computed-goto dispatch is a
   jump to a label in `run()`. A case cannot become a function at all without
   giving up the dispatch strategy.
2. The helpers *could* have been separate translation units, and were, for one
   commit. Measured against the interpreter benchmarks that cost **13–49%** —
   branches +49%, methods +29%, locals +25%, `loop_arith` +23%, classes +19% —
   because the compiler had been inlining `peekStack`, `frameModule`,
   `propertyReadSlow` and `callValue` into the dispatch loop and stopped.
   Restoring one translation unit put it back: an interleaved A/B of the two
   binaries came out between −1.9% and +2.7%, which is noise.

An `.inc` is **C source, not a header**: no include guards, no declarations,
just definitions — and for the `vm_ops_*` set, `switch` case bodies that are
only legal inside `run()`. The Makefile globs `*.c`, so none of them is ever
compiled on its own, and each is listed in `vm.d` so editing one rebuilds
`vm.o`.

Two pieces of the VM did not need this and are ordinary translation units,
because neither is on the hot path:

| File | Holds |
| --- | --- |
| [`vm_fiber.c`](vm_fiber.c) | Suspendable calls: `await`, generators, and the stack each gets |
| [`vm_event.c`](vm_event.c) | The microtask queue, timers, and the loop that drains them |
| [`vm_internal.h`](vm_internal.h) | The seams between those three |

## Everything else

| Values | Holds |
| --- | --- |
| [`value.c`](value.c) | What a `Value` is, and what it converts to |
| [`value_render.c`](value_render.c) | Turning one into text, in the two ways that differ |
| [`value_internal.h`](value_internal.h) | The seam between those two |

| Objects | Holds |
| --- | --- |
| [`object.c`](object.c) | The allocation every heap object goes through, and strings |
| [`object_new.c`](object_new.c) | The constructors for everything that is not a property bag |
| [`object_bag.c`](object_bag.c) | An object's properties: slots while the layout is shared, a table once it is not |
| [`object_print.c`](object_print.c) | How each object prints, which is not how it converts |
| [`object_gc.c`](object_gc.c) | What the collector does with each object type |
| [`object_internal.h`](object_internal.h) | The one thing they all do: register with the collector |
| [`shape.c`](shape.c) | Hidden classes: the layout objects built the same way share |
| [`table.c`](table.c) | The open-addressing hash table all of the above use |

| The rest | Holds |
| --- | --- |
| [`memory.c`](memory.c) | The allocator, and the mark-sweep collector behind it |
| [`module.c`](module.c) | Resolving, loading and ordering source files |
| [`bigint.c`](bigint.c) | Arbitrary-precision integers |
| [`regex.c`](regex.c) | Compiling a pattern to the program the matcher runs |
| [`regex_match.c`](regex_match.c) | Running one against a subject, backtracking on an explicit trail |
| [`regex_internal.h`](regex_internal.h) | The compiled program both halves see |

## Three hazards this directory is full of

**Every allocation can collect.** Anything reachable only from a C local has
to be pushed as a temporary root first, which is what the `csPushTempRoot`
calls everywhere are for. The subtle version is ordering: a count published
before the array it describes exists leaves the collector walking memory that
is not there yet, which is why `shape.c` sets `slotCount` last and
`chunk.c` publishes its cache count last.

**A fast path and its miss path must agree exactly.** The fast paths live in
the `vm_ops_*.inc` bodies and the slow ones in
[`vm_cache_miss.inc`](vm_cache_miss.inc); a shape hit that answers one thing
and a miss that answers another is a bug that appears only under a cache
eviction, which is to say almost never, which is to say in production.

**Adding an opcode touches three places.**
[`../../include/cscript/opcode.h`](../../include/cscript/opcode.h) declares it
once and generates both the enum and the dispatch table, so those cannot
drift — but the body goes in whichever `vm_ops_*.inc` owns its role, and the
disassembler in [`../compiler/debug.c`](../compiler/debug.c) needs a case for
its operand layout, which nothing will catch for you.

## Related

- [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md#memory) — the collector, the shapes, the inline caches, and the measurements behind each
- [../../docs/PERFORMANCE.md](../../docs/PERFORMANCE.md) — what the loop costs against Node
- `make test-gc` collects on every allocation; `make test-switch` builds the portable dispatch; `make test-tagged` swaps NaN-boxing for a tagged union
