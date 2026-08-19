# CScript grammar and semantics — v0.14.0

CScript's syntax is a **subset of TypeScript's**: every CScript program is a
valid TypeScript program. `make test-node` enforces that by handing each
example to Node with `--experimental-strip-types`, which erases the annotations
and runs the JavaScript underneath, then diffing the output. That checks both
halves of the claim at once — the syntax really is TypeScript, and the
behaviour really does match once the types are gone.

The semantics are not a subset. CScript removes several of JavaScript's
best-known traps. Every divergence is listed in
[Fixes to JavaScript](#fixes-to-javascript), and every one of them is a **loud
error**, never a silent change of meaning — a program that would behave
differently here fails to compile or fails at runtime instead.

## Grammar

Written in EBNF. `*` is zero or more, `?` is optional, `|` is alternation.

```ebnf
program        = statement* EOF ;

statement      = varDeclaration
               | block
               | ifStatement
               | whileStatement
               | forStatement
               | expressionStatement ;

varDeclaration = ( "let" | "const" ) IDENTIFIER typeAnnotation?
                 ( "=" expression )? ";" ;
typeAnnotation = ":" TYPE_NAME ;
block          = "{" statement* "}" ;
ifStatement    = "if" "(" expression ")" statement ( "else" statement )? ;
whileStatement = "while" "(" expression ")" statement ;
forStatement   = "for" "(" ( varDeclaration | expressionStatement | ";" )
                           expression? ";" expression? ")" statement ;
expressionStatement = expression ";" ;

expression     = assignment ;

assignment     = IDENTIFIER ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) assignment
               | logicalOr ;

logicalOr      = logicalAnd ( "||" logicalAnd )* ;
logicalAnd     = equality   ( "&&" equality )* ;
equality       = comparison ( ( "===" | "!==" ) comparison )* ;
comparison     = term       ( ( "<" | "<=" | ">" | ">=" ) term )* ;
term           = factor     ( ( "+" | "-" ) factor )* ;
factor         = unary      ( ( "*" | "/" | "%" ) unary )* ;

unary          = ( "!" | "-" | "typeof" | "++" | "--" ) unary
               | postfix ;

postfix        = primary ( "." IDENTIFIER | "(" arguments? ")" | "++" | "--" )* ;
arguments      = expression ( "," expression )* ;

primary        = NUMBER | STRING | IDENTIFIER
               | "true" | "false" | "null" | "undefined"
               | "(" expression ")" ;
```

## Operator precedence

Loosest binding first. Binary operators are left-associative; assignment and
the unary operators are right-associative.

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` | right |
| 2 | `\|\|` | left |
| 3 | `&&` | left |
| 4 | `===` `!==` | left |
| 5 | `<` `<=` `>` `>=` | left |
| 6 | `+` `-` | left |
| 7 | `*` `/` `%` | left |
| 8 | `!` `-` `typeof` `++` `--` (prefix) | right |
| 9 | `.` `( )` `++` `--` (postfix) | left |

## Types

Five of JavaScript's primitives. `symbol` and `bigint` do not exist.

| Type | Literals | `typeof` |
| --- | --- | --- |
| number | `42`, `3.14`, `1e3`, `1.5e-2`, `0x1F` | `"number"` |
| string | `"double"`, `'single'` | `"string"` |
| boolean | `true`, `false` | `"boolean"` |
| null | `null` | `"null"` ← **differs from JavaScript** |
| undefined | `undefined` | `"undefined"` |

### Numbers

All numbers are IEEE 754 doubles; there is no integer type.

- Integral values print without a decimal point: `10 / 2` prints `5`
- `1 / 0` is `Infinity`, `0 / 0` is `NaN`
- `%` takes the sign of the left operand: `-7 % 3` is `-1`
- Number-to-string follows ECMA-262 exactly: the shortest decimal that reads
  back as the same double, switching to exponent notation only once the decimal
  point falls outside `(-6, 21]`. So `1e20` prints in full and `1e21` does not;
  `1e-6` prints in full and `1e-7` does not.
- `console.log(-0)` shows `-0`, while `String(-0)` gives `"0"` — the same split
  JavaScript makes between `util.inspect` and `String`

At most 65536 distinct constants may appear in one function.

### Strings

Immutable and interned — two strings with the same contents are the same
object, so equality is a pointer comparison.

Escapes: `\n` `\t` `\r` `\0` `\\` `\'` `\"`. An unrecognised escape keeps the
character as written. Template literals are not implemented.

## Types

CScript is **gradually typed**. Annotations are optional; a declaration without
one takes the type of its initialiser, so unannotated code is still checked.

```js
const name: string = "cscript";   // annotated
let year = 2026;                  // inferred as number — just as checked
let loose: any = 1;               // opted out of static checking
```

| Type name | Matches |
| --- | --- |
| `number` | all numeric values |
| `string` | |
| `boolean` | |
| `null` | |
| `undefined` | |
| `object` | `console`, `Math` |
| `any` | anything — the escape hatch |

`any` is assignable in both directions. It is the one place the checker
deliberately stops being sound, and it is what makes the system gradual rather
than static.

### What the checker catches

All of these fail **before the program runs**, and most need no annotation at
all because the types are inferred from literals:

```js
let total: number = "text";   // cannot assign string to 'total', declared as number
let n = 1; n = "text";        // cannot assign string to 'n', which is number
true * 3;                     // operand of '*' must be a number, got boolean
let n = 1; let s = "1";
n === s;                      // the types can never match
let c = 1; c();               // number is not a function
let n = 1; n.field;           // cannot read property 'field' of number
let x: integer = 1;           // unknown type 'integer'
```

### Deliberate gaps

**Comparing against `null` or `undefined` is always allowed**, even when the
other side has a known, different type. Those checks are idiomatic, and without
union types there is no way to write "a string, or null" — so rejecting them
would punish correct code for a hole in the type system.

**Object properties are dynamic.** `Math.PI` type-checks as `any`, because
object shapes are not modelled yet.

**Function signatures do not exist yet.** A call's result is `any`. Both arrive
with user-defined functions.

**The type system is deliberately shallow** — a fixed set of primitives, no
structural or higher-order types. That is what keeps gradual typing cheap here.
Sound gradual type systems get expensive at the boundary between typed and
untyped code, because a function or object crossing it must be wrapped in a
contract that checks every later use; Typed Racket measured 10–100× slowdowns
from exactly that. A primitive needs one check, or none.

## Variables

`let` and `const` only, both block-scoped.

```js
let count = 0;
const limit = 10;

count += 5;
count++;

{
  let count = 99;   // shadows the outer one
  console.log(count);
}
```

- `const` must be initialised, and cannot be reassigned or updated
- Redeclaring a name in the same scope is an error
- Reading or assigning a name that was never declared is an error
- The built-ins (`console`, `Math`, `Number`, …) are constants

## Semantics

### Truthiness

Six values are falsy: `false`, `null`, `undefined`, `0`, `NaN`, `""`.
Everything else is truthy. Identical to JavaScript.

### Equality

`===` and `!==` compare type first — values of different types are never equal.
`NaN === NaN` is `false`, per IEEE 754.

There is no coercing equality. See below.

### `+`

If **either** operand is a string, both are converted to strings and
concatenated. Otherwise both must be numbers.

```js
1 + 2          // 3
"a" + "b"      // "ab"
"n = " + 42    // "n = 42"
true + " x"    // "true x"
true + 1       // runtime error
```

### `&&` and `||`

Both short-circuit and evaluate to **one of their operands**, not to a boolean.
Identical to JavaScript.

```js
"value" || "unused"   // "value"
null || "fallback"    // "fallback"
"first" && "second"   // "second"
0 && "never"          // 0
```

### `++` and `--`

Prefix yields the value after the update; postfix yields the value before it.
Identical to JavaScript.

```js
let a = 5;
console.log(a++, a);   // 5 6
let b = 5;
console.log(++b, b);   // 6 6
```

---

## Fixes to JavaScript

Each of these is a place where JavaScript's behaviour is a known source of bugs.
CScript changes it — and makes the change **visible**, so nothing silently means
something different than it would in JavaScript.

### 1. No coercing equality

`==` and `!=` are rejected at compile time.

```js
1 == "1"
// error: '==' is not supported because it coerces its operands; use '==='
```

`==` follows a coercion table almost nobody has memorised, which is why
essentially every JavaScript style guide bans it. Rather than silently making
`==` mean `===` — which would let a program mean something different here than
in Node — CScript refuses to compile it and names the fix.

### 2. `typeof null` is `"null"`

```js
typeof null   // "null"   (JavaScript: "object")
```

This is the one divergence that is **not** an error, because there is no way to
make it one. It is a bug from JavaScript's first week that cannot be fixed
without breaking the web. CScript is not bound by that.

### 3. No `var`

```js
var x = 1;
// error: 'var' is not supported because it is function-scoped and hoisted;
//        use 'let' or 'const'
```

`var` is function-scoped and hoisted to the top of its function, so it can be
read before its declaration and leaks out of blocks. `let` and `const` do
neither.

### 4. Arithmetic does not coerce

`-` `*` `/` `%` require numbers. JavaScript coerces and usually yields `NaN`,
which then spreads silently through every downstream calculation until something
prints `NaN` a long way from the actual mistake.

```js
true * 3
// runtime error: operands of '*' must be numbers, got boolean and number
```

`+` is the exception: string concatenation is too useful to give up.

### 5. Undeclared variables are errors

```js
console.log(typo);   // runtime error: 'typo' is not defined
typo = 5;            // runtime error: 'typo' is not defined
```

Sloppy-mode JavaScript returns `undefined` for the read and silently creates a
global for the write, turning a typo into a bug that surfaces somewhere else.

### 6. Constants are actually constant

```js
const answer = 42;
answer = 43;   // error: 'answer' is declared const and cannot be reassigned
console = 5;   // runtime error: 'console' is a constant and cannot be reassigned
```

### 7. No automatic semicolon insertion

Semicolons are required. ASI is a rewriting pass over your source that changes
what the program means, most famously by inserting one after a bare `return`.

### 8. No implicit `ToNumber`

Unary `+` is rejected; use `Number(x)`. Conversions are explicit and readable:
`Number(x)`, `String(x)`, `Boolean(x)`.

---

## Built-ins

| Name | Members |
| --- | --- |
| `console` | `log`, `error`, `warn` |
| `Math` | `floor`, `abs`, `max`, `min`, `PI`, `E` |
| `Number(x)` | explicit conversion to number |
| `String(x)` | explicit conversion to string |
| `Boolean(x)` | explicit conversion to boolean |
| `NaN`, `Infinity` | the usual numeric constants |

All of them are constants and cannot be reassigned.

## Comments

```js
// to end of line

/* block comments,
   which may span lines */
```

Block comments do not nest.

## Standard library

Method calls go through one instruction that looks the name up on the receiver,
so arrays and strings carry methods without being property bags.

**Arrays** — `push` `pop` `shift` `unshift` `slice` `concat` `join` `indexOf`
`lastIndexOf` `includes` `reverse` `fill` `sort` `forEach` `map` `filter`
`reduce` `find` `findIndex` `some` `every`

`sort` compares as strings by default, so `[10, 9].sort()` is `[10, 9]`.
Surprising, but specified, and real code depends on it. Pass a comparator for
numeric order.

**Strings** — `toUpperCase` `toLowerCase` `trim` `trimStart` `trimEnd` `split`
`slice` `substring` `charAt` `charCodeAt` `indexOf` `lastIndexOf` `includes`
`startsWith` `endsWith` `repeat` `replace` `replaceAll` `padStart` `padEnd`
`concat`

Indexing is by byte, which is correct for ASCII and wrong for multi-byte UTF-8.

**Namespaces** — `console.log` / `.error` / `.warn`; `Object.keys` / `.values` /
`.entries` / `.assign` / `.hasOwn`; `Array.isArray` / `.of` / `.from`;
`Number.isInteger` / `.isNaN` / `.isFinite` / `.parseInt` / `.parseFloat` and
the numeric limits; `JSON.stringify` / `.parse`; twenty-five `Math` functions
and constants.

**Globals** — `Number` `String` `Boolean` `Error` `parseInt` `parseFloat`
`isNaN` `isFinite` `NaN` `Infinity`.

`Math.random` is seeded once from the clock and is not cryptographic.

## Error handling

```js
try {
  throw Error("something failed");
} catch (e) {
  console.log(e.name, e.message);
} finally {
  console.log("always runs");
}
```

Any value can be thrown. `Error(message)` returns `{ name, message }` — a
function rather than a constructor, since `new` does not exist yet.

`finally` runs on every path out of the block, including `return`, `break` and
`continue` leaving it early, and it covers the `catch` as well as the body.

An uncaught throw is reported like a runtime error, with the same call stack.

## Destructuring and spread

```js
const [first, ...rest] = [1, 2, 3];
const { x, y: renamed, z = 0 } = point;

console.log([...a, ...b]);
console.log(Math.max(...numbers));
```

Array and object patterns, with renaming, defaults and a rest element. Spread
works in array literals and call arguments, and over strings.

Not supported, each with an error that says so: **nested patterns**,
**destructuring a parameter** (destructure inside the body instead), **object
rest**, and spreading into a built-in method call such as `xs.push(...ys)` —
packing the arguments loses the receiver those need.

## Classes

```ts
class Account {
  balance = 0;              // field with an initialiser
  owner;                    // field without one — still part of the layout

  constructor(owner: string) {
    this.owner = owner;
  }

  deposit(amount: number) {
    this.balance = this.balance + amount;
    return this;            // methods chain
  }

  static open(owner: string) {
    return new Account(owner);
  }
}

class Savings extends Account {
  rate = 0.05;

  constructor(owner: string) {
    super(owner);           // required, and required to come first
  }

  describe(): string {
    return `${super.describe()} (savings)`;
  }
}

new Savings("Ada") instanceof Account;   // true
```

Fields, a constructor, methods, `static` methods, `extends`, `super`, `this`
and `instanceof`.

**Instances are ordinary objects** with a class attached. Fields are properties
in declaration order, so `Object.keys`, `JSON.stringify` and printing all work
without a special case, and instances share a layout the same way object
literals do.

**Field initialisers run where JavaScript runs them**: at the top of a base
class's constructor, and directly after `super(...)` in a derived one. That
ordering is observable through key order, which is why it is matched exactly
rather than simplified.

**`this` is slot 0 of a method's frame**, which means an arrow function inside a
method captures it through the ordinary closure machinery and gets JavaScript's
lexical `this` with no rule of its own. `this` outside a class method is an
error rather than `undefined`.

**A method keeps its receiver.** `const f = obj.method; f()` works — in
JavaScript it loses `this`, which is the reason `.bind(this)` exists. This is a
deliberate divergence; see `examples/fixes.cx`.

Two rules are stricter than JavaScript's, both to keep the order above legible:
a subclass constructor must call `super(...)` as its **first** statement, and a
constructor cannot `return` a value.

Not supported, each with an error that says so: **getters and setters**,
**private `#fields`**, **static fields**, **static blocks**, **computed member
names**, **`new.target`**, and **subclassing built-ins**.

Class names are not usable as type annotations. The type lattice is a fixed set
of primitives, so an instance is `object` and a class is dynamic; nominal types
are the next typing milestone rather than part of this one.

## Modules

One file, one module, its own top-level scope. Nothing a file declares escapes
it unless the file says so.

```ts
// math.cx
export const PI = 3.14159;
export function add(a: number, b: number): number { return a + b; }
export class Vec { … }

const helper = 1;
export { helper };

// main.cx
import { add, PI } from "./math.cx";
import { add as plus } from "./math.cx";
import * as math from "./math.cx";
```

A specifier is a **relative path with the extension written out**. There is no
package system to resolve a bare name against, and guessing extensions is how
a module system starts needing a resolver nobody can predict.

Everything a file imports is loaded, compiled and run before the file itself,
so a program's compile errors all surface in one pass and a module has already
finished by the time anything reads from it. A file is read once no matter how
many others import it.

An import is a **live read** of the exporting module's binding, not a copy of
its value at import time. A namespace object exposes exactly the exported
names, sorted, and is frozen.

**Cycles are an error**, reported at the import that closed the loop. ES
modules answer a cycle with a half-initialised namespace and a `ReferenceError`
if you touch the wrong thing at the wrong moment; refusing it names the problem
where it is.

Not supported, each with an error that says so: **default exports** (one file,
two ways to name a thing), **`export *`**, **re-exporting** with
`export { x } from "..."`, **dynamic `import()`**, and **bare specifiers**.
Imported bindings are `any` to the type checker — types do not cross a file
boundary yet.

## Converting to a string

An array converts the way JavaScript does: its elements joined with commas,
nested arrays flattened, `null` and `undefined` contributing nothing. So
`` `${[1, [2, 3]]}` `` is `"1,2,3"` and `String([])` is `""`.

An object does **not**. JavaScript gives `[object Object]`, which says nothing
about the object and is a byword for a bug that reached the screen; CScript
gives the same rendering `console.log` would. This is a deliberate divergence
and the only one in string conversion.

Printing and converting are separate paths on purpose: `console.log([1, 2])`
shows `[ 1, 2 ]` so nested structure stays readable, while `"" + [1, 2]` is
`"1,2"`.

## Asynchrony

```ts
async function fetchUser(id: number) {
  await sleep(5);
  return { id: id, name: `user${id}` };
}

const users = await Promise.all([fetchUser(1), fetchUser(2)]);
```

`async function`, `async () => {}` and `async m() {}` in a class body. An async
function hands its caller a promise immediately and carries on where it left
off once whatever it awaited settles. `return` fulfils that promise, a throw
rejects it, and a rejection arrives at the `await` as an exception — so
`try`/`catch`/`finally` work unchanged.

**Promises.** `new Promise((resolve, reject) => …)`, `Promise.resolve`,
`.reject`, `.all`, `.race`, and `.then` / `.catch` / `.finally`. Settling a
promise *queues* its handlers rather than running them, which is why `.then`
is asynchronous even on an already settled promise, and is what makes ordering
predictable.

**The loop.** Everything synchronous runs first, then the microtask queue is
drained completely, then one timer gets a turn, and so on until both are
empty. `setTimeout`, `clearTimeout` and `queueMicrotask` are the way in.

A rejection nothing ever listened to is reported and exits non-zero, as it
does in Node — a promise that failed with no one watching is a bug, and the
alternative is a program that silently does half its work.

**Top-level `await` is out**, with an error that says so: it would make
running a module itself asynchronous and reorder every import in a program.
Generators, `for await`, and async iterators are not implemented either.

## Not implemented yet

Each of these produces an error that names it:

- Regular expressions
- `Map`, `Set`, `Symbol`, `BigInt`
- `do`/`while`, labelled statements, `for...in`
- Object-literal shorthand — write `{ x: x }` rather than `{ x }`
- Computed property keys
