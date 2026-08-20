# CScript against JavaScript

Every CScript program is a valid TypeScript program, and — once the type
annotations are stripped — almost every one produces byte-identical output
under Node. That is not a claim, it is a build step: `make test-node` runs all
fourteen examples through both and fails if any of them disagree, except the
one that exists to demonstrate the disagreements.

This document is the full account of where the two languages meet and where
they part. Three kinds of entry:

- **Same** — behaves as JavaScript does, and is tested against it.
- **Differs** — behaves differently *on purpose*, with the reason.
- **Missing** — not implemented, and says so when you write it.

---

## 1. What is the same

Everything in this list is checked against Node by the test suite rather than
asserted here.

### Values and operators

| | |
| --- | --- |
| Numbers | IEEE 754 doubles, including `NaN`, `Infinity` and `-0` |
| Number formatting | ECMA-262 `Number::toString` — `1e21`, `1e-7`, `0.1 + 0.2` |
| Strings | immutable, interned, `+` concatenates |
| Template literals | `` `a${b}c` ``, nested |
| `typeof` | for every type except `null` — see *Differs* |
| Arithmetic | `+ - * / % **`, with `**` right-associative |
| Comparison | `< <= > >=`, `=== !==` |
| Logical | `&& ||` evaluate to an operand, not a boolean |
| Nullish coalescing | `??`, and mixing it with `&&`/`||` without parentheses is an error, as in JavaScript |
| Logical assignment | `&&= ||= ??=`, short-circuiting — no store when the operator says not to |
| Conditional | `c ? a : b`, right-associative |
| Truthiness | `0`, `""`, `NaN`, `null`, `undefined`, `false` are falsy |
| `instanceof` | walks the class chain |
| `in` | own properties, class methods, and an array's indices |
| `delete` | on properties and computed keys |

### Syntax

| | |
| --- | --- |
| Declarations | `let`, `const`, several per statement |
| Functions | declarations, expressions, arrows, closures |
| `this` | lexical in arrows, receiver in methods |
| Classes | fields, methods, `static`, getters and setters, `extends`, `super` |
| Private members | `#field`, `#method()`, `static #field` — off the shape, so invisible to `Object.keys`, `JSON.stringify` and subscripts |
| Static blocks | `static { … }`, interleaved with static fields in source order |
| Destructuring | array and object, nested, defaults, renaming, rest elements and rest properties |
| Parameters | patterns as parameters, in every function form |
| Spread | array literals, object literals, call arguments, over strings |
| Object literals | shorthand `{ x }`, computed keys `{ [k]: v }`, numeric keys `{ 1: v }`, methods `{ m() {} }`, spread `{ ...o }` |
| Optional chaining | `?.`, `?.[ ]`, `?.()`, short-circuiting the whole chain |
| Control flow | `if`, `while`, `do`/`while`, `for`, `for...of`, `for...in`, `switch` |
| `break` / `continue` | including labelled, and out of `try` with a `finally` |
| Labelled statements | on loops, on `switch`, and on blocks |
| Exceptions | `try`/`catch`/`finally`/`throw`, any value throwable |
| Modules | `import`/`export`, named, default, namespace, `export *`, re-exports, per-file scope |
| Asynchrony | `async`/`await`, promises, `setTimeout`/`setInterval`, the microtask queue |
| Promise combinators | `all`, `race`, `allSettled`, `any` — with `AggregateError` |
| Generators | `function*`, `yield`, `yield*`, `next(v)`, `for...of`, spread |
| Async generators | `async function*`, `next()` answering with a promise, `for await` |
| Top-level `await` | In any file, including one another file imports |
| `for await` | Over an async generator, and over any sync iterable, awaiting each element |

### The standard library

`console`, `Math` (25 functions and constants), `JSON.stringify` / `.parse`,
`Object.keys` / `.values` / `.entries` / `.assign` / `.hasOwn`, `Array.isArray`
/ `.of` / `.from`, `Number` and its limits, `parseInt`, `parseFloat`, `isNaN`,
`isFinite`, `Promise` with `.resolve` / `.reject` / `.all` / `.race`, `Map` and
`Set`, regular expressions, 21 array methods and 21 string methods.

`Map` and `Set` key by SameValueZero and iterate in insertion order, including
after deletions — both checked against Node.

Ordering is part of this. Settling a promise *queues* its reactions rather than
calling them; `.finally` and resolving a promise with a promise each cost the
microtask turn the specification says they cost; the loop drains microtasks
completely before running a timer. A seven-chain ordering test is
byte-identical to Node — that is the part implementations usually get wrong.

---

## 2. What differs, and why

Nine deliberate divergences. Every one is demonstrated in
[`examples/fixes.cx`](../examples/fixes.cx), which the parity check *requires*
to differ from Node.

### `typeof null` is `"null"`

JavaScript says `"object"`. That is a bug from 1995 that cannot be fixed
without breaking the web. CScript is not bound by that.

### `==` and `!=` do not exist

Not "are discouraged" — they are a **compile error** naming the fix. Coercing
equality is the single largest source of JavaScript's surprising comparisons,
and once `===` exists there is nothing the coercing form buys.

### `var` does not exist

A compile error. Function-scoped and hoisted is a family of bugs; `let` and
`const` cover the ground.

### Types are checked before the program runs

With or without annotations. `true * 3` and `count === "41"` are compile
errors. JavaScript would produce `NaN` and `false` and carry on. This is the
one difference that changes what *fails*, rather than what a working program
prints — a program Node accepts may be rejected here, and that is the point.

### Conversions are explicit

`Number("42")`, `String(42)`, `Boolean("")`. No implicit numeric coercion of a
boolean or a string in arithmetic. String concatenation with `+` still works,
because that one is unambiguous.

### The standard library is read-only

`Math.PI = 3` and `console.log = f` are errors at the line that writes them.
JavaScript allows both, and the damage shows up somewhere else entirely. A user
object with the same key is unaffected.

### An object converted to a string shows its contents

`` `${{ x: 1 }}` `` gives `{ x: 1 }`, not `[object Object]` — a string that
says nothing and is a byword for a bug that reached the screen. **Arrays match
JavaScript exactly**: `String([1, [2, 3]])` is `"1,2,3"`, because that one is
useful rather than a wart.

### A method keeps its receiver

`const f = obj.method; f()` works. In JavaScript `this` is lost and the call
throws, which is why `.bind(this)` is scattered through real code. Reading a
method here produces a bound method.

### An unhandled promise rejection is fatal

As it is in Node — listed because it is a deliberate choice rather than an
accident. A promise that failed with nobody watching is a bug, and the
alternative is a program that silently does half its work.

### The async iterator protocol is generators only

`for await` drives an async generator or any sync iterable. It does not look
for a `Symbol.asyncIterator` method on an arbitrary object, because `Symbol`
does not exist here — so an object cannot make itself async-iterable the way
one can in JavaScript.

### A generator's `.return()` does not run a pending `finally`

```js
function* g() { try { yield 1; } finally { console.log("cleanup"); } }
const it = g();
it.next();
it.return(9);   // JavaScript prints "cleanup" first; CScript does not
```

Abandoning a suspended generator drops its fiber, and the fiber owns its whole
stack, so nothing leaks — but the `finally` blocks on that stack never run.
Running them would mean resuming the body in a mode that unwinds without
continuing, which the fiber machinery does not have yet. `break` out of a
`for...of` over a generator has the same gap, because it does not call
`.return()` at all.

### Known, not yet fixed

**Property order for integer-like keys.** JavaScript enumerates `{ b: 1, 2: 2 }`
as `2, b` — integer-like keys first, ascending, then the rest in insertion
order. CScript uses insertion order throughout. Uncommon in idiomatic code,
since an integer-keyed object is usually an array, but it is a real difference
and it is written down here rather than left to be discovered.

---

## 3. What is missing

Each of these produces an error that names it, rather than failing obscurely.

| Missing | Note |
| --- | --- |
| Tagged template literals | Ordinary template literals work |
| Regex lookbehind — `(?<=…)` — and named groups | Lookahead and backreferences work |
| `WeakMap`, `WeakSet` | Would need weak references in the collector |
| `yield*` inside a larger expression | Works as a statement of its own; a delegate's return value is not available |
| `Symbol`, `BigInt` | No plans |
| Getters and setters on object *literals* | Methods and shorthand work; accessors do not |
| Computed member names in a class body | Computed keys work in object literals |
| `new.target`, subclassing built-ins | |
| Prototypes, `__proto__`, `Object.create` | Classes are the whole object model |
| `arguments`, `Function.prototype.call` / `.apply` / `.bind` | A method already keeps its receiver |
| Dynamic `import()` | Static `import` and `export` are resolved before the program runs |
| Bare import specifiers — `import x from "lodash"` | No package system to resolve against |
| Sparse arrays and holes | Deliberate: they are why engines need a second array representation |
| Unicode-correct string indexing | Strings are indexed by byte, which is correct for ASCII |
| `with`, `eval` | No plans |

Class names are not usable as type annotations. The type lattice is a fixed set
of primitives, so an instance is `object` and a class is dynamic. Types do not
cross a module boundary either — an imported binding is `any`. Nominal types
are the next typing milestone rather than part of this one.

---

## 4. Where CScript is faster, and where it is not

Measured, not asserted; `bench/run.sh` prints this table and the README carries
the current numbers.

CScript is a bytecode interpreter with inline caches and hidden classes. Node
is a tracing JIT with two optimising compilers behind it.

On compute — process startup measured and subtracted — CScript runs **8× to
34× slower than Node**, and about 35× slower than the same loop in C or Go.

CScript *starts* about 18× faster: 2 ms against Node's 37 ms. For a script that
runs for a few milliseconds that dominates, which is why `bench/run.sh` reports
end-to-end time as well. Neither number alone is the answer.

The gap is narrowest on array work, where most of the time is in the same kind
of C either way, and widest on property-heavy code, where V8's inline caches
sit behind code it has specialised and recompiled.

What is written down in the README, because it was measured rather than
predicted: computed-goto dispatch was worth 0%, NaN-boxing was worth 0% on
speed (though it halves memory), hidden classes and inline caches were worth
11–13% against an expected 20–40%, and fusing the most frequent opcode pairs
showed that removing an instruction here buys about a third of an instruction —
which prices a register VM at 10–20% rather than the usual 20–40%.

---

## 5. Trying it

```bash
make
./build/release/cscript examples/fixes.cx        # the divergences, running
node --experimental-strip-types examples/fixes.cx
make test-node                                   # every example, both ways
```
