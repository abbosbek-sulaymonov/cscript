# Roadmap

What has been built, in the order it was built, and what is next. A row marked
next is a decision already made rather than a wish — usually because something
already measured says it is the thing that pays.

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
| **49 ✅** | Property descriptors, and own accessors that enumerate | — |
| **50 ✅** | Regex lookbehind and named groups, with `.groups` and `$<name>` | — |
| **51 ✅** | `Date` setters, `Date.UTC`, `Date.parse`, and `new.target` | — |
| **52 ✅** | Dynamic `import()`, and one namespace object per module | — |
| **53 ✅** | Guessed parameter types with a guard, so ordinary calls compile | — |
| next | Calling a CScript function from compiled code, which needs frames and safepoints | — |
| next | Inlining, so a small function is worth compiling | — |
| **54 ✅** | Property reads lowered against a shape, checked once at entry | — |
| **55 ✅** | Property reads emitted in machine code, both bytecode forms lowered | — |
| **56 ✅** | Method calls answered from compiled code, with the receiver at entry | — |
| **57 ✅** | Property writes lowered and emitted, and a back-edge that stops re-asking | — |
| **58 ✅** | `new` consults the compiler — and why no constructor yet qualifies | — |
| next | A property store that *adds* one, which is what a constructor does | — |
| next | Allocating an object in compiled code — attempted, backed out; see below | — |

---

## Allocation in compiled code: what an attempt found

Tried and withdrawn, because it produced a wrong answer on `bench/properties`
that was not diagnosed. What it turned up is worth keeping.

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

The wrong answer survived all of the above being fixed, so there is at least
one more thing wrong, and the honest position is that it is not yet understood.
