# Roadmap

What has been built, in the order it was built, and what is next. A row marked
next is a decision already made rather than a wish — usually because something
already measured says it is the thing that pays.

| Milestone | Adds |
| --- | --- |
| **1 ✅** | Lexer, parser, compiler, VM, GC, REPL, expressions |
| **2 ✅** | `let`/`const`, scopes, control flow, calls, `console.log` |
| **3 ✅** | Typing: annotations, inference, checking |
| **4 ✅** | User functions, `return`, closures, typed signatures |
| **5 ✅** | Object literals, arrays, indexing, `.length` |
| **6 ✅** | `switch`, `break`/`continue`, template literals, ternary |
| **7 ✅** | NaN-boxed values — halves memory, measured |
| **8 ✅** | Superinstructions chosen from an opcode profile |
| **9 ✅** | Method dispatch, array and string methods |
| **10 ✅** | `Object`, `Array`, `Number`, `JSON`, full `Math` |
| **11 ✅** | `**`, arrow functions, `for...of` |
| **12 ✅** | `try` / `catch` / `finally` / `throw` |
| **13 ✅** | Destructuring and spread |
| **14 ✅** | Hidden classes and inline caches for properties and globals |
| **15 ✅** | `class`, `new`, `this`, `extends`, `super`, `instanceof` |
| **16 ✅** | Modules — `import`, `export`, per-file scope |
| **17 ✅** | Promises, timers, the event loop, `async`/`await` |
| **18 ✅** | The rest of the syntax: patterns, accessors, `do`/`for...in` |
| **19 ✅** | Split the three largest files; documented against JavaScript |
| **20 ✅** | Tiering: what gets hot, and how much of it is already typed |
| **21 ✅** | `Map` and `Set`, patterns as `for...of` bindings |
| **22 ✅** | Regular expressions: literals, `test`/`exec`, string methods |
| **23 ✅** | A typed IR, verified by running it instead of the interpreter |
| **24 ✅** | An arm64 backend — correct, verified, and not yet faster |
| **25 ✅** | Register allocation: −5% on calls, still level on loops |
| **26 ✅** | On-stack replacement — loops **8.9× faster**, 1.6× of Node |
| **27 ✅** | `?.`, `??`, `&&=`/`\|\|=`/`??=`, object-literal methods |
| **28 ✅** | `in`, `delete`, object spread and rest, labelled statements |
| **29 ✅** | `#private` members, `static { }` blocks |
| **30 ✅** | Default exports, re-exports, `export *` |
| **31 ✅** | Generators — `function*`, `yield`, `yield*`, pull-driven `for...of` |
| **32 ✅** | Top-level `await`, `for await` over sync iterables |
| **33 ✅** | Regex backreferences and lookahead, function replacers, `exec().index` |
| **34 ✅** | `Promise.allSettled` / `.any`, `AggregateError`, `setInterval` |
| **35 ✅** | Async generators — `async function*`, `for await` over one |
| **36 ✅** | Side exits — the compiler takes the loop and leaves the rest |
| **37 ✅** | Globals in a compiled loop — `loop_empty` **8.6×**, `globals` **5.2×** |
| **38 ✅** | Calling out — `%` compiles, with nothing spilled around the call |
| **39 ✅** | Default parameters, class expressions, `at`/`flat`/`flatMap`, number formatting |
| **40 ✅** | Rest parameters, `call`/`apply`/`bind`, tagged templates |
| **41 ✅** | Computed class members, `new` on any expression, object-literal accessors |
| **42 ✅** | `Date` — the last large missing built-in |
| **43 ✅** | `WeakMap` and `WeakSet`, with ephemeron marking in the collector |
| **44 ✅** | `Symbol`, symbol-keyed properties, and `Symbol.iterator` |
| **45 ✅** | `BigInt` — arbitrary precision, and a checked `number` boundary |
| **46 ✅** | Prototypes — `Object.create`, `__proto__`, and a chain reads walk |
| **47 ✅** | Constructor functions — `new F()`, `F.prototype`, and call-site `this` |
| **48 ✅** | Inherited and static accessors, `constructor`, a `toString` that is called |
| **49 ✅** | Property descriptors, and own accessors that enumerate |
| **50 ✅** | Regex lookbehind and named groups, with `.groups` and `$<name>` |
| **51 ✅** | `Date` setters, `Date.UTC`, `Date.parse`, and `new.target` |
| **52 ✅** | Dynamic `import()`, and one namespace object per module |
| **53 ✅** | Guessed parameter types with a guard, so ordinary calls compile |
| **54 ✅** | Property reads lowered against a shape, checked once at entry |
| **55 ✅** | Property reads emitted in machine code, both bytecode forms lowered |
| **56 ✅** | Method calls answered from compiled code, with the receiver at entry |
| **57 ✅** | Property writes lowered and emitted, and a back-edge that stops re-asking |
| **58 ✅** | `new` consults the compiler — and why no constructor yet qualifies |
| **59 ✅** | Inlining — a small callee's body goes where the call was, **16.7×** |
| **60 ✅** | `===` on anything but a number, which compiled code got wrong |
| **61 ✅** | Replaying a hand-over, so a loop below a declaration still compiles — `jit_calls` **23.7×**, `calls` **24.0×** |
| **62 ✅** | A branch that read its condition from memory the value was never in |
| **63 ✅** | Slot types per block rather than per function |
| **64 ✅** | Replaying a jump, so a hand-over with a `break` in it keeps the loop |
| **65 ✅** | The command line: `--check`, stage dumps, `process.argv`, and a REPL that prints |
| **66 ✅** | Every source file under 600 lines — 24 of them were over, one by 4515 |
| **67 ✅** | The standard library: 39 modules under `std:`, each a directory with its specification |
| **68 ✅** | No `any`: one-time autocasting, a checked top type with `typeof` narrowing, and parameters that must say |
| **69 ✅** | `interface` and `type`, checked structurally and erased — and one top type, spelled `unknown` |
| **70 ✅** | A type tree: `T[]`, `(a: A) => B`, `A \| B`, and generics with inference at the call site |
| **71 ✅** | A class's name as a type — and the base-class fields `super()` never ran |
| **72 ✅** | Types across a module boundary: what a file exports arrives knowing what it is |
| **73 ✅** | The built-in generics — `Map<K, V>`, `Set<T>`, `Promise<T>`, `Array<T>` — and `await` that unwraps one |
| **74 ✅** | Literal types, `keyof`, `T[K]`, mapped types, and the six utility types made of them |
| **75 ✅** | A property store that *adds* one — a constructor reaches machine code, **1.45×** |
| **76 ✅** | Allocating an object in compiled code — `bench/properties` **1.6×**, and what the backed-out attempt had missed |
| **77 ✅** | Calling a CScript function from compiled code, for the callees inlining will not take — **5.0×** on `bench/jit/jit_calls_out` |
| **78 ✅** | Deoptimisation — an exit builds the frame it resumes in, and happens where it is forced rather than at the last empty stack |
| **79 ✅** | Guards at the site — a shape checked where it is used, and every hot function in the corpus lowers |
| next | Speculative guards: a type *guessed* rather than proved, so the arithmetic the checker could not settle still compiles |

---

## Two holes that replaying a jump found

Neither was reachable while a hand-over containing a jump ended the lowering,
and both were older than any of this.

**The reachability walk did not follow fall-through.** A block the lowering
cuts short of a terminator runs into the next one, which the code generator
relies on when it lays the blocks out in order — but the walk that proves no
compiled path reaches a block with a *fabricated* entry height only followed
jumps and branches. So such a block could be declared unreachable and then be
reached, handing the interpreter a frame at the wrong depth. `labels.cx` did
exactly that, and the answer was a boolean where a loop counter should be.

**A block picked up after a skip carried a stale belief about its slots.** The
lowering would resume at a block whose height some jump had recorded — the
height being a fact — while its own model of what each slot held still
described the path it had abandoned. Nothing had noticed because the model is
only advisory: every load it types optimistically is retyped by the dataflow
afterwards, and a function whose arithmetic loses its proof is refused. It is
still the wrong thing to record as a path that happened, so a block resumed
this way now takes its types from a predecessor or claims nothing at all —
which is what made it worth teaching every lowered jump to record its types
along with its height.

---

## Allocation in compiled code: what the attempt missed

Tried once and withdrawn, because it produced a wrong answer on
`bench/properties` that was not diagnosed. It is in now, and the thing it had
missed was not on the list below — see the end.

**The collector cannot see a compiled frame.** On a call entry the slots are a
local array in `csJitTryRun`; on the OSR path they are the interpreter's frame,
but compiled code writes past what the interpreter has opened and `markRoots`
walks only as far as `stackTop`. Harmless only while compiled code cannot
allocate — which is exactly the condition being removed. The fix is small,
though: every value compiled code holds is a number except the objects in frame
slots, so what is needed is a **root range**, not a stack map.

**There is nowhere to put a temporary.** Compiled code needs a slot to hold a
freshly allocated object where the collector can see it, and `IrFunction`'s
`slotCount` is not it — that is the highest slot the *lowering happened to
touch*, which lands on a local the lowering did not. `ObjFunction` records no
frame size, so one has to be added, from the compiler's local high-water mark.

**Three passes have to be told that allocation is a store.**
`csIrReconcileSlotTypes` and `promotableSlots` both walk only `IR_STORE_LOCAL`,
so a slot written by an allocation looked unwritten, was typed a number, and
got a floating-point register — after which the property store into it was
refused. `csIrRemoveDeadStores` counts a slot as read only through
`IR_LOAD_LOCAL`, so a slot read through a property access looks dead; that one
is now hardened, and it is the only part of the attempt that was kept.

All of the above is now done: the root range is `VM.jitRoots`, the slots an
allocation reads and writes are named by `csIrWritesSlot` and `csIrReadsSlots`
rather than inferred from stores, and the three passes ask those instead.

**What the attempt had missed was a fourth pass, and a silence.** The
forwarding pass — the one that turns a store followed by a load of the same
slot into a register rename — tracked slots by looking for `IR_STORE_LOCAL`
too. A slot written by an allocation looked untouched to it as well, so a load
*after* the allocation was renamed to the register holding what the slot had
held *before*. That is a wrong answer with nothing to see: no crash, no
refusal, no diagnostic, just the previous iteration's object read back.

The silence was worse. The machine-code emitter's switch had no `default`, so
an instruction it did not know emitted **nothing at all** — the allocation
simply did not happen, and whatever the slot held before was read as an object.
That is how the first version of this change segfaulted rather than saying it
could not compile the function. The switch refuses now, which costs the
`-Wswitch` warning that would have named a new opcode and buys a compiler that
cannot silently skip one.

---

## Calling out of compiled code: three things and one measurement

Inlining answers a call by not making one, which works for a callee whose body
is straight-line and small. A callee with a loop of its own has no body to
splice, and until this one call handed the whole of the caller back to the
interpreter — `digitSum` in `bench/jit/jit_calls_out` is four lines long and
cost the outer loop everything.

**The frame is the operand stack.** A call needs the callee's arguments where
the interpreter would have left them, which is the slots above the callee's own
position, and leaves its result in that position. Doing it that way means an
exit anywhere downstream needs nothing put back: the frame after a compiled
call looks exactly as it would have after an interpreted one.

**The caller's frame has to stay rooted while the callee runs.** `VM.jitRoots`
was one range, replaced for the length of a run. A compiled function that calls
another compiled one has two live frames, and replacing would have unrooted the
caller — so it is a stack of ranges now, pushed and popped around each entry.
Nothing else changed in `markRoots`.

**A callee that throws ends the compiled frame.** There is no offset to resume
at: the callee already ran, and re-entering the caller would run it again. So a
failed call takes an exit whose recorded offset is `CS_JIT_EXIT_FAILED`, and
both entry points — `csJitTryRun` and `csJitOsr` — report it as `failed` rather
than as a frame handed back. The interpreter then takes the throw where it
stands, through the same `HANDLE_FAILED_CALL` a native callback uses.

Two things went wrong on the way, and both were silent.

**The splice's rollback took the arguments with it.** `OP_CALL` tries inlining
first and drops what the attempt emitted when it does not take. The mark it
rolled back to was recorded *before* the arguments came off the abstract stack
— and taking one may emit a load. So the fallback stored registers whose
defining instructions had just been deleted, and the callee was handed whatever
those registers happened to hold. `digitSum` was called with garbage and
answered 0, which is a plausible-looking wrong answer. The mark is recorded
after the arguments now.

**The call was slower than not making it.** `csVMCallCallback` — what a native
uses to call back into user code — goes straight to `callClosure`, skipping the
entry that consults the compiler. A compiled callee reached that way is
*interpreted*, so the first working version of this ran `jit_calls_out` in
346 ms against the 293 ms it took when the caller was handed back to the
interpreter and only the callee compiled. `csVMCallFromCompiled` goes through
`callValue` instead, which is the ordinary entry: 170 ms, and 5.0× against the
same binary with the compiler off.

One thing is knowingly lost: a compiled frame is not on `vm.frames`, so a stack
trace taken inside a callee does not name the compiled caller.

---

## Deoptimisation: two things an exit could not do

An exit hands the frame back at a bytecode offset and the interpreter carries
on from there. It has existed since stage 5, and two restrictions on it meant
most of what it was for did not happen.

**A body with an exit in it could not be entered by a call.** On a back-edge
entry the interpreter's own frame is what compiled code runs on, so handing it
back costs nothing. A call entry has no frame at all — the slots are a local
array belonging to the run — so there was nothing to hand back, and `csJitTryRun`
refused any function with an exit anywhere in it. One unsupported opcode in the
tail of a body meant every call to it was interpreted from its first
instruction. 40 of the 261 functions the corpus compiles were in that position.

The frame is built on the way out now. `csVMDeoptimise` pushes the frame the
call would have got, copies the compiled slots into it, points the instruction
pointer at the exit's offset and sets the operand stack to the exit's height.
Only for a call whose arity is exact: a missing argument, a rest parameter and
a default are all filled in by something that runs *before* the offset being
resumed at, and would be done twice.

**An exit rewound to the last point the operand stack was empty.** The reason
given was that a value pushed since then lives in a register the interpreter
has no name for. It does not: `csIrPush` stores into the slot as well, because
in this VM the operand stack and the frame are the same array. So every
position below the current height is already where the interpreter reads it
from, and the exit can be at the instruction that forced it, with only that one
instruction's emitted work dropped.

What the old rewind cost is easiest to see in a body of straight-line
declarations. `const` leaves its value on the stack, so the stack never returns
to the block's floor, so the floor never moves off the block's start — and a
hand-over anywhere in such a body threw away the whole block. A function of
eight arithmetic statements ending in an array literal lowered to exactly one
instruction: `exit -> bytecode 0`.

The one position that is not materialised is a callee placeholder, which stands
for a global the lowering resolved and never pushed. A hand-over with one live
still rewinds to the floor, which is below it by construction.

| | Compiled | Calls answered |
| --- | ---: | ---: |
| `bench/jit/jit_deopt` before | 133 ms | 0 |
| `bench/jit/jit_deopt` after | **61 ms** | 1,990,000 |

The differential suite went from 52 programs the compiler takes part in to 59,
and from 77 reaching machine code to 83; under a collection at every allocation,
from 29–30 and 54 to 38 and 60.

What this does **not** add is a speculative guard — an exit is still only ever
forced by something the compiler has no encoding for, never by a type it
guessed. The mechanism a guard needs is what was missing, and it is here: a
guard can now fail anywhere in a body, on any entry, and the interpreter picks
the frame up at that instruction.

---

## Guards at the site: what an entry assumption cannot say

A property read is lowered against the layout its inline cache has settled on,
and that layout was checked *once*, at entry, before the body started. One
check per call, for free at the read — and it is still the right answer for a
slot the caller filled.

It is the wrong answer for a slot the **body writes**. The clearest case is an
object built inside the function:

    function boxed(n: number): number {
      const made = { a: n, b: n + 1 };
      return made.a + made.b;
    }

At entry `made`'s slot holds nothing at all, so the entry check failed on every
single call and the whole call was interpreted — for the sake of a check that
could never have held. `IR_GUARD_SHAPE` asks the same question at the read,
where it is true, and deoptimises when it is not: the frame goes back to the
interpreter at the property instruction, which then does the read its own way.
That is the first thing the deoptimisation work was for.

Which route a site takes is decided by whether anything lowered so far has
written the slot. The bytecode question — is there an assignment to it? — was
the one being asked, and it misses this entirely: a local *is* its stack
position in this VM, so `const p = { … }` assigns nothing and the slot looked
untouched. `csIrWritesSlot` knows better, and is what decides now.

A second read of the same object needs no second guard, provided nothing wrote
the slot in between. That is tracked per block, because a guard proves nothing
about a path that did not go through it.

## And the refusals that were never refusals

The same file was refusing whole functions for one property it could not
compile. A cache that saw the property **absent** — reached through a prototype,
or not there at all — names no layout to read from and none to guard on, and
that was 20 of the 26 hot functions the corpus refused outright.

Nothing about it warranted refusing the function. With an exit that happens at
the instruction, the frame simply goes back to the interpreter there and the
rest of the body still compiles. Every property and object case that could
refuse now hands over instead.

| | Before | After |
| --- | ---: | ---: |
| Hot functions lowered to typed IR | 249 of 275 | **275 of 275** |
| Reaching machine code | 70 | **86** |
| `bench/jit/jit_guards`, compiled | 389 ms | **278 ms** |
| …calls answered by compiled code | 0 | 2,000,000 |
| `make test-jit` takes part in / machine code | 59 / 83 | **61 / 88** |
| `make test-jit-gc` takes part in / machine code | 38 / 60 | **40 / 66** |

The guard costs a call out to `csJitShapeHolds`, which is why a function with
one in it allocates only from the callee-saved bank — the same restriction
`IR_MOD` and the object builder already imposed. Inlining that check into arm64
is the obvious next thing to try on it, and was left out here deliberately: the
first version of a guard should be one whose correctness is readable.
