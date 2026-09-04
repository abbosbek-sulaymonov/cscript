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

| Feature | How it behaves |
| --- | --- |
| Numbers | IEEE 754 doubles, including `NaN`, `Infinity` and `-0` |
| BigInt | `123n`, `0xffn`, arbitrary precision, and refusing to mix with a number |
| Number formatting | ECMA-262 `Number::toString` — `1e21`, `1e-7`, `0.1 + 0.2` |
| Strings | immutable, interned, `+` concatenates |
| Template literals | `` `a${b}c` ``, nested |
| `typeof` | for every type except `null` — see *Differs* — including `"symbol"` and `"bigint"` |
| Arithmetic | `+ - * / % **`, with `**` right-associative |
| Comparison | `< <= > >=`, `=== !==` |
| Logical | `&&` and `\|\|` evaluate to an operand, not a boolean |
| Nullish coalescing | `??`, and mixing it with `&&`/`\|\|` without parentheses is an error, as in JavaScript |
| Logical assignment | `&&=` `\|\|=` `??=`, short-circuiting — no store when the operator says not to |
| Conditional | `c ? a : b`, right-associative |
| Truthiness | `0`, `""`, `NaN`, `null`, `undefined`, `false` are falsy |
| `instanceof` | walks the class chain |
| `in` | own properties, class methods, and an array's indices |
| `delete` | on properties and computed keys |

### Syntax

| Feature | What is supported |
| --- | --- |
| Declarations | `let`, `const`, several per statement and per `for` initialiser |
| Classes as values | `const C = class { … }`, named or not |
| Numeric separators | `1_000_000`, `0xFF_FF` |
| `void` and the comma operator | `void x`, `(a, b)` — the operator, not the separator |
| Functions | declarations, expressions, arrows, closures |
| `this` | lexical in arrows, receiver in methods |
| Classes | fields, methods, `static`, getters and setters, `extends`, `super`, computed member names |
| `new` | on any expression that names a class, not only a name |
| Private members | `#field`, `#method()`, `static #field` — off the shape, so invisible to `Object.keys`, `JSON.stringify` and subscripts |
| Static blocks | `static { … }`, interleaved with static fields in source order |
| Destructuring | array and object, nested, defaults, renaming, rest elements and rest properties |
| Parameters | patterns, defaults and `...rest`, in every function form |
| `call` / `apply` / `bind` | including partial application, on any callable |
| Tagged templates | `` tag`a${x}b` ``, with `raw` |
| Spread | array literals, object literals, call arguments, over strings |
| Object literals | shorthand `{ x }`, computed keys `{ [k]: v }`, numeric keys `{ 1: v }`, methods `{ m() {} }`, accessors `{ get x() {} }`, spread `{ ...o }` |
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
| `for await` | Over an async generator, over any sync iterable awaiting each element, and over any object offering `Symbol.asyncIterator` |

### The standard library

`console`, `Math` (25 functions and constants), `JSON.stringify` / `.parse`,
`Object.keys` / `.values` / `.entries` / `.fromEntries` / `.assign` /
`.hasOwn` / `.freeze` / `.isFrozen` / `.getOwnPropertyNames` / `.create` /
`.getPrototypeOf` / `.setPrototypeOf` / `.defineProperty` / `.defineProperties`
/ `.getOwnPropertyDescriptor` / `.getOwnPropertyDescriptors`, `Array.isArray`
/ `.of` / `.from` — the latter over an array, a string, a Map or Set, a
generator or an array-like, with an optional mapping function — `Number` and
its limits, `parseInt`, `parseFloat`, `isNaN`,
`isFinite`, `Promise` with `.resolve` / `.reject` / `.all` / `.race` /
`.allSettled` / `.any`, `AggregateError`, `setTimeout` / `setInterval` and
their cancellers, `Map` and `Set`, regular expressions, 25 array methods, 22
string methods, and `toFixed` / `toPrecision` / `toString(radix)` on numbers.
`Symbol` with its registry and the well-known `Symbol.iterator` and
`Symbol.asyncIterator`; `BigInt` with `toString(radix)` and `valueOf`.

A function answers `length` — how many arguments it must be given, so a
parameter with a default is not among them — and `name`.

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

This is only about an object with no `toString` of its own. One that has a
`toString` — declared on a class, written on a literal, or inherited from a
prototype — has it called, exactly as JavaScript does. Inspection is a separate
question and shows the contents either way, which is what Node's `console.log`
does with such an object too.

### A method keeps its receiver

`const f = instance.method; f()` works for a class method and for one reached
through a prototype: reading either produces a bound method. In JavaScript
`this` is lost and the call throws, which is why `.bind(this)` is scattered
through real code.

The rule is about *reading* a method. Calling one always supplies the receiver,
whatever kind of function it is: `o.f()` passes `o` even when `f` is a plain
function that happens to say `this`, which is what makes
`Point.prototype.sum = function () { ... }` work.

Reading stops at *own* methods — `const f = ({ m() {} }).m` is the plain
function. An own method lives in the object's shape and so is served by the
inline cache, and binding it would mean testing every property read in the VM
for whether it happened to produce a method. A class method and an inherited
one can never be in the receiver's own shape, so binding them costs nothing
that anyone else pays.

Calling is unaffected either way: `o.m()` and `o[key](...)` both keep the
receiver, whatever kind of method they find.

### There is no root `Object.prototype`

`Object.getPrototypeOf({})` is `null` here and `Object.prototype` in
JavaScript, and for the same reason `({}).constructor` is `undefined` rather
than the `Object` builtin. `constructor` does answer for anything actually
built by something: a class instance names its class, and an object made with
`new F()` names `F`. It is not enumerable, so `Object.keys(F.prototype)` is
empty here as it is in JavaScript. A plain object starts with no prototype because the methods that
would live on a root — `hasOwnProperty`, `toString`, `valueOf` — are not
properties of any object here: `Object.hasOwn` asks the same question as a
static, and how a value renders is the language's answer rather than a method
a program can replace.

The consequence is small and worth naming: a chain you build yourself behaves
exactly as it does in JavaScript, but it ends one link earlier.

### An unhandled promise rejection is fatal

As it is in Node — listed because it is a deliberate choice rather than an
accident. A promise that failed with nobody watching is a bug, and the
alternative is a program that silently does half its work.

### A `Date` writes itself as ISO, and reads a time with no zone as UTC

`String(date)` gives `1970-01-01T00:00:00.000Z` rather than JavaScript's local,
human, locale-shaped form — the same string `toISOString` and JSON give. And
`new Date("2024-01-31T12:00:00")`, with no `Z`, is read as UTC where JavaScript
reads it as local time.

Both are the same choice: a program's output should not depend on where it is
running. The local zone is still available where it is asked for by name —
`getHours`, `getDay` and `getTimezoneOffset` all use it, and so does
`new Date(y, m, d)`.

### A runtime error is not catchable

`try`/`catch` catches what a program `throw`s. It does not catch reading a
property of `null`, calling something that is not a function, or handing a
`WeakMap` a key it cannot hold — those stop the program and print where.

JavaScript makes every one of those a `TypeError` that a `catch` may swallow,
which is how a typo becomes a silent branch. Here the two kinds of failure stay
separate: a `throw` is a decision the program made, and a runtime error is a
mistake in it.

### Regular expressions

Lookbehind — `(?<=…)` and `(?<!…)` — and named groups `(?<name>…)`, with
`.groups` on a match and `$<name>` in a replacement, alongside the lookahead,
backreferences, classes and quantifiers that were already there.

The engine is a backtracker and cannot run a program backwards, so a lookbehind
does the other thing that gives the same answer: it tries every start position
at or before the cursor, nearest first, and requires the body to finish exactly
there. That costs a scan proportional to how far back it looks, which for the
assertions people write is a handful of bytes — and it is correct for a body of
any shape, which a fixed-width calculation would not be.

`$<name>` follows JavaScript's asymmetry: a pattern with no names at all leaves
`$<` as text, and one that has names treats an unknown name as an empty
capture.

### Dynamic `import()`

`import(specifier)` names a module at run time and answers a promise for its
namespace. The specifier is an expression, so it may be computed.

Reading and compiling a file is synchronous here, so the promise is already
settled when it is handed back. That is a difference in *when* the work happens
rather than in what a program can observe: the result is still a promise, still
reached through `await` or `.then`, and still delivered on a microtask. What it
does change is ordering between two imports started in the same turn — theirs
settle as the I/O completes, ours in source order.

A module that cannot be found or read rejects rather than stopping the program,
which is the whole point of asking for one at run time. An error *inside* the
module is a runtime error and keeps travelling, because runtime errors are not
catchable here.

A module whose own top level awaits cannot be imported this way: settling it
would mean running the event loop from inside the call that asked for it, and
naming that beats handing back a namespace of names that are not there yet.
Importing such a module statically works, because the loader drives it in
dependency order before anything that needs it runs.

A namespace is one object per module, so `import * as a` and `await import(…)`
of the same file give the same object — as they do in JavaScript.

### `new.target`

`new.target` is whatever `new` was applied to, and undefined for a plain call —
which is what makes a constructor function able to refuse being called without
`new`. It is per call rather than per function, so a call *made by* a
constructor answers undefined: the value does not travel down the stack.

Inside an arrow it is a compile error. JavaScript has an arrow borrow it from
what encloses it, the way an arrow borrows `this`; here the answer lives on the
frame and an arrow pushes one of its own, so naming the gap beats quietly
giving the wrong value.

### Date

Every setter — `setFullYear`, `setMonth`, `setDate`, `setHours`, `setMinutes`,
`setSeconds`, `setMilliseconds`, `setTime` and the seven `setUTC…` forms — plus
`Date.UTC` and `Date.parse`.

A setter may write several components at once, as JavaScript's do:
`setFullYear(y, m, d)` is one call that writes three. Out-of-range components
roll over rather than being rejected, because putting a date back together is
the same arithmetic as taking one apart — a 32nd of December is the 1st of
January. A NaN anywhere makes the whole date invalid and it stays that way.

### Property descriptors

`Object.defineProperty`, `defineProperties`, `getOwnPropertyDescriptor`,
`getOwnPropertyDescriptors` and `Object.create`'s second argument all work,
with `value`, `writable`, `enumerable`, `configurable`, `get` and `set`.

An attribute a descriptor leaves out is false rather than inherited, so
`Object.defineProperty(o, "x", { value: 1 })` makes a property that is hidden
and read-only. That surprises people and is exactly what JavaScript specifies.

A property written the ordinary way has all three attributes, so there is
nothing to record about it: an object grows the side table these live in only
once `defineProperty` has said otherwise about one of its names. Every other
object pays one pointer test per enumerated key and nothing at all on a read.

Writing to a read-only property is refused rather than dropped, and deleting or
redefining a non-configurable one is too — the same three answers an ES module
gives, and for the same reason a store that silently does nothing is worth
refusing.

What is *not* modelled is a descriptor on anything but an ordinary object:
array indices, `length`, and the built-in namespaces do not take one.

### Own accessors take a slot, so that they enumerate

An accessor written on an object — `{ get b() {} }`, or defined through a
descriptor — lives on a hidden class the object gets for itself, which is where
the property paths already look. Reading and writing one therefore costs
nothing at all for every object that has none.

That used to mean an accessor was invisible to `Object.keys`, JSON and a
spread, and this document argued the alternative was not worth its price: a
slot in the object's shape holding a marker, and a test for that marker on
every property read in the program.

The price turned out to be avoidable. The name does take a slot holding a
stand-in, which is what puts it in the insertion order — but the object also
leaves shape mode when it gains one, so it is never served by an inline cache
and the test for the stand-in sits on the slow path, which such an object was
always going to take. Objects with no accessors are untouched; the cost falls
on the ones that have them, which is where it belongs.

So `Object.keys`, `Object.values`, `Object.entries`, `JSON.stringify`, a spread
and `Object.assign` all see own accessors now, in the order they were written,
and reading their values runs the getter. Inspecting one prints `[Getter]`,
`[Setter]` or `[Getter/Setter]` rather than running it, because looking at an
object must not have side effects — which is what Node prints too.

A class's accessors are still not enumerated on an instance. They belong to the
class rather than to the object, which is where JavaScript puts them as well.

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

### A `number` annotation is checked when `any` crosses into it

The checker enforces an annotation wherever it can see the argument's type, but
`any` is assignable to everything by design — that is what makes the system
gradual. So a value that arrived through an untyped edge reaches an annotated
parameter unexamined, and the annotation is not advice: arithmetic on a
`number` parameter compiles to an unchecked add, and the JIT seeds the slot as
a number and stops guarding.

CScript therefore checks a `number` parameter against its argument on the way
in, and reports `argument 1 is bigint but the parameter is number` rather than
reading the value's bits as a double. JavaScript has no annotations to check
and TypeScript erases its own, so this has no counterpart in either; it is the
contract a sound gradual boundary needs, and it is one comparison per annotated
parameter per call — too small to measure on a call-heavy benchmark.

### Known, not yet fixed

**Property order for integer-like keys.** JavaScript enumerates `{ b: 1, 2: 2 }`
as `2, b` — integer-like keys first, ascending, then the rest in insertion
order. CScript uses insertion order throughout. Uncommon in idiomatic code,
since an integer-keyed object is usually an array, but it is a real difference
and it is written down here rather than left to be discovered.

### BigInt

`123n` is a whole number with no upper bound, and a type of its own rather than
a wider number. Arithmetic is exact however far it goes — `2n ** 128n` is the
whole answer, not a rounded one — and `typeof` says `"bigint"`.

Mixing a BigInt with a number under an arithmetic operator is refused, exactly
as JavaScript refuses it. That looks unhelpful until you see the alternative:
widening the BigInt to a double throws away precisely the precision it exists
to keep, and narrowing the number has to invent an answer for `1n + 0.5`.
Refusing is the only choice that cannot be wrong. Where both operands are
literals CScript reports it before the program runs; otherwise the VM does.

Ordering mixes freely — `1n < 2` has exactly one right answer — and is decided
exactly, without rounding either side. `9007199254740993n > 9007199254740992`
is `true`, which is not a comparison a double could make.

Equality is by value, which makes a BigInt the one heap type not compared by
identity. `1n === 1` stays `false`: they are different types.

The `n` is how a BigInt is *written*, not what it says. `String(7n)` and
`` `${7n}` `` are both `"7"`; printing one shows `7n`, as Node does.

Not implemented: the bitwise operators and shifts on BigInts, which CScript has
for no type, and `BigInt.asIntN` / `asUintN`, which exist to truncate to a
width the language has no other use for.

The arithmetic is sign-and-magnitude over 32-bit limbs, with division done one
bit at a time. Knuth's algorithm D is much faster and much easier to get subtly
wrong; the numbers a script divides are small, and being able to read the code
and believe it is worth more than the constant factor here.

### Constructor functions

`function Point(x) { this.x = x }` with `new Point(1)` works, and so does the
whole classic pattern built on it: `Point.prototype.method = ...`,
`Child.prototype = Object.create(Parent.prototype)`, `Parent.call(this, ...)`
to delegate, and `instanceof` against the function.

What makes a function a constructor is the `new`, not anything about how it was
written — JavaScript's rule exactly. `this` follows from that: `new F()` puts
the object being built in it, a call through a property puts the receiver
there, `call`/`apply`/`bind` put what they were given, and a bare call leaves
it undefined. A constructor may return a different object, and anything that is
not an object is discarded in favour of what was being built.

`F.prototype` is created the first time something asks for it, because most
functions are never constructed from and an object per closure would be a cost
every call pays for a feature most calls do not use.

Only the top level of a module has no `this`, and that is a compile error
rather than `undefined` — the same answer, said earlier.

### Prototypes

An object may inherit from another. `Object.create(proto)`, `__proto__` — read
and written, and honoured in an object literal — `Object.getPrototypeOf` and
`Object.setPrototypeOf` all work, and a read that misses walks the chain.

Only reads walk it. A write always creates or updates an *own* property, which
is what makes a prototype a shared default rather than shared storage, and
everything that enumerates — `Object.keys`, JSON, a spread — sees own
properties only. So `in` and `Object.hasOwn` genuinely differ here, as they do
in JavaScript: the first walks the chain and the second is exactly the question
that does not.

A method found on a prototype is bound to the object it was reached *through*,
not the one it was found on, which is what makes `this` mean the instance.

Classes are not built on this. A class keeps its own method table and its own
dispatch, so `Object.getPrototypeOf(instance)` is `null` rather than
`Class.prototype`, and a class's methods cannot be reached or replaced through
one. Rebuilding classes on prototypes would put a chain walk in front of every
method call that the class model answers with a single table lookup; the two
mechanisms coexist instead, and an instance may still be given a prototype.

The inline caches are untouched by any of this. A cache hit means the name was
found in the object's own shape, which an inherited property never is — so an
inherited read takes the slow path every time. That is the honest cost of not
encoding the prototype in the shape.

Setting a prototype that is already in the object's chain is refused, because
the loop it would make has no lookup that terminates. JavaScript refuses it
too.

---

## 3. What is missing

Each of these produces an error that names it, rather than failing obscurely.

| Missing | Note |
| --- | --- |
| `yield*` inside a larger expression | Works as a statement of its own; a delegate's return value is not available |
| `Date`'s locale formats | `toLocaleString` and `toLocaleDateString`. Every non-ISO input form is implementation-defined in JavaScript too, so `Date.parse` of one is NaN |
| `arguments` | Deliberate, and now for a stronger reason than taste: arity is checked, so a call may not pass more arguments than the function declares. There are never extra arguments for `arguments` to collect, and it could only ever repeat the parameters. A rest parameter is how you accept a variable number, and it says so in the signature |
| Subclassing built-ins — `class MyArray extends Array` | `extends` takes a class, and the built-ins are native constructors rather than classes; the error says so |
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
