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
| BigInt | `123n`, `0xffn`, arbitrary precision, and refusing to mix with a number |
| Number formatting | ECMA-262 `Number::toString` — `1e21`, `1e-7`, `0.1 + 0.2` |
| Strings | immutable, interned, `+` concatenates |
| Template literals | `` `a${b}c` ``, nested |
| `typeof` | for every type except `null` — see *Differs* — including `"symbol"` and `"bigint"` |
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
| `for await` | Over an async generator, and over any sync iterable, awaiting each element |

### The standard library

`console`, `Math` (25 functions and constants), `JSON.stringify` / `.parse`,
`Object.keys` / `.values` / `.entries` / `.fromEntries` / `.assign` /
`.hasOwn` / `.freeze` / `.isFrozen` / `.getOwnPropertyNames` / `.create` /
`.getPrototypeOf` / `.setPrototypeOf`, `Array.isArray`
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

### An object literal's accessors are not enumerated

```js
const o = { a: 1, get b() { return 2; } };
o.b;                 // 2, as in JavaScript
Object.keys(o);      // [ 'a' ] here; [ 'a', 'b' ] in JavaScript
JSON.stringify(o);   // {"a":1} here; {"a":1,"b":2} in JavaScript
```

An accessor lives on a hidden class the object gets for itself, which is where
the property paths already look — so reading and writing one costs nothing at
all for every object that has none. The alternative is a slot in the object's
shape holding a marker, and a test for that marker on every property read in
the program.

Property reads are the hot path and already the widest gap against Node, so
that tax is not worth paying for a feature whose point is the reading rather
than the listing. It is a real difference and it is here rather than buried.

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
| Regex lookbehind — `(?<=…)` — and named groups | Lookahead and backreferences work |
| `yield*` inside a larger expression | Works as a statement of its own; a delegate's return value is not available |
| `Date` parsing beyond ISO, and its locale formats | `toLocaleString`, `Date.parse` of anything else, `setFullYear` and the other setters |
| `Object.defineProperty` and property descriptors | Writability, enumerability and configurability are not modelled, so `Object.create` refuses a second argument rather than ignoring it |
| `arguments` | A rest parameter does the same job and says what it collects |
| `new.target`, subclassing built-ins | |
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
