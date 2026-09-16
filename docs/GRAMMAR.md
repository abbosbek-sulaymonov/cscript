# CScript grammar and semantics — v0.21.0

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

statement      = varDeclaration | destructuring
               | functionDecl | classDecl
               | importDecl | exportDecl
               | block
               | ifStatement | whileStatement | doWhileStatement
               | forStatement | forOfStatement
               | switchStatement | tryStatement
               | labelledStatement
               | emptyStatement
               | "break" IDENTIFIER? ";" | "continue" IDENTIFIER? ";"
               | "throw" expression ";"
               | "return" expression? ";"
               | expressionStatement ;

varDeclaration = ( "let" | "const" ) declarator ( "," declarator )* ";" ;
emptyStatement = ";" ;
declarator     = IDENTIFIER typeAnnotation? ( "=" expression )? ;
destructuring  = ( "let" | "const" ) pattern "=" expression ";" ;
typeAnnotation = ":" TYPE_NAME ;

pattern        = "[" arrayElement? ( "," arrayElement? )* "]"
               | "{" objectEntry  ( "," objectEntry  )* "}" ;
arrayElement   = ( IDENTIFIER | pattern ) ( "=" expression )?
               | "..." IDENTIFIER ;
objectEntry    = IDENTIFIER ( ":" ( IDENTIFIER | pattern ) )? ( "=" expression )?
               | "..." IDENTIFIER ;                    (* rest property *)

functionDecl   = "async"? "function" "*"? IDENTIFIER
                 "(" parameters? ")" typeAnnotation? block ;
parameters     = parameter ( "," parameter )* ;
parameter      = "..."? ( IDENTIFIER typeAnnotation? | pattern ) ( "=" expression )? ;

classDecl      = "class" IDENTIFIER ( "extends" IDENTIFIER )? "{" member* "}" ;
member         = "static" block                        (* static initialiser *)
               | "static"? ( method | field ) ;
method         = ( "get" | "set" )? memberName "(" parameters? ")"
                 typeAnnotation? block
               | ( "async" | "*" ) memberName "(" parameters? ")"
                 typeAnnotation? block ;
field          = memberName typeAnnotation? ( "=" expression )? ";" ;
memberName     = propertyName | PRIVATE_NAME | "[" expression "]" ;
propertyName   = IDENTIFIER | KEYWORD ;

importDecl     = "import" importClause "from" STRING ";" ;
importClause   = namedImports
               | "*" "as" IDENTIFIER
               | IDENTIFIER ( "," ( namedImports | "*" "as" IDENTIFIER ) )? ;
namedImports   = "{" importName ( "," importName )* "}" ;
importName     = IDENTIFIER ( "as" IDENTIFIER )? ;

exportDecl     = "export" ( varDeclaration | functionDecl | classDecl
                          | namedImports ( "from" STRING )? ";"
                          | "*" "from" STRING ";"
                          | "default" ( functionDecl | classDecl
                                      | expression ";" ) ) ;

block          = "{" statement* "}" ;
labelledStatement = IDENTIFIER ":" statement ;
ifStatement    = "if" "(" expression ")" statement ( "else" statement )? ;
whileStatement = "while" "(" expression ")" statement ;
doWhileStatement = "do" statement "while" "(" expression ")" ";" ;
forStatement   = "for" "(" ( declarator ( "," declarator )* ";"
                           | expressionStatement | ";" )
                           expression? ";" expression? ")" statement ;
forOfStatement = "for" "await"? "(" ( "let" | "const" )
                 ( IDENTIFIER | pattern ) ( "of" | "in" )
                 expression ")" statement ;
switchStatement = "switch" "(" expression ")" "{"
                  ( "case" expression ":" statement* )*
                  ( "default" ":" statement* )? "}" ;
tryStatement   = "try" block ( "catch" ( "(" IDENTIFIER ")" )? block )?
                 ( "finally" block )? ;
expressionStatement = expression ";" ;

expression     = sequence ;
sequence       = assignment ( "," assignment )* ;   (* the comma operator *)
assignment     = ( IDENTIFIER | property | index )
                 ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "**="
                 | "&&=" | "||=" | "??=" ) assignment
               | conditional ;
conditional    = coalesce ( "?" assignment ":" conditional )? ;

(* `??` sits on the same tier as `||` and may not be mixed with `||` or `&&`
   without parentheses, which is a rule rather than a precedence. *)
coalesce       = logicalOr | logicalAnd ( "??" logicalAnd )+ ;
logicalOr      = logicalAnd ( "||" logicalAnd )* ;
logicalAnd     = equality   ( "&&" equality )* ;
equality       = comparison ( ( "===" | "!==" ) comparison )* ;
comparison     = term ( ( "<" | "<=" | ">" | ">=" | "instanceof" | "in" ) term )* ;
term           = factor     ( ( "+" | "-" ) factor )* ;
factor         = exponent   ( ( "*" | "/" | "%" ) exponent )* ;
exponent       = unary      ( "**" exponent )? ;          (* right-associative *)

unary          = ( "!" | "-" | "typeof" | "await" | "void" | "++" | "--" ) unary
               | "delete" ( property | index )
               | "yield" "*"? assignment?
               | postfix ;
postfix        = primary ( "." propertyName
                         | "?." propertyName
                         | "[" expression "]"
                         | "?." "[" expression "]"
                         | "(" arguments? ")"
                         | TEMPLATE                    (* a tagged template *)
                         | "?." "(" arguments? ")"
                         | "++" | "--" )* ;
arguments      = argument ( "," argument )* ;
argument       = "..."? expression ;

primary        = NUMBER | BIGINT | STRING | TEMPLATE | REGEX | IDENTIFIER
               | "true" | "false" | "null" | "undefined"
               | "this" | "super" ( "." propertyName | "(" arguments? ")" )
               | "new" ( IDENTIFIER | "(" expression ")" )
                       ( "." propertyName | "[" expression "]" )*
                       ( "(" arguments? ")" )?
               | arrayLiteral | objectLiteral | arrowFunction
               | "async"? "function" "*"? IDENTIFIER? "(" parameters? ")" block
               | "class" IDENTIFIER? ( "extends" IDENTIFIER )? "{" member* "}"
               | "(" expression ")" ;

arrayLiteral   = "[" ( argument ( "," argument )* )? "]" ;
objectLiteral  = "{" ( objectProperty ( "," objectProperty )* )? "}" ;
objectProperty = propertyName                         (* shorthand: { x } *)
               | propertyName ":" expression
               | STRING ":" expression
               | NUMBER ":" expression                (* { 1: v } *)
               | "[" expression "]" ":" expression    (* computed key *)
               | "*"? propertyName "(" parameters? ")" block  (* method *)
               | ( "get" | "set" ) propertyName "(" parameters? ")" block
               | "..." expression ;                   (* spread *)
arrowFunction  = "async"? ( IDENTIFIER | "(" parameters? ")" typeAnnotation? )
                 "=>" ( expression | block ) ;
```

## Operator precedence

Loosest binding first. Binary operators are left-associative; assignment and
the unary operators are right-associative.

| Level | Operators | Associativity |
| --- | --- | --- |
| 0 | `,` (the operator) | left |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `**=` `&&=` `\|\|=` `??=` | right |
| 2 | `? :` | right |
| 3 | `\|\|` `??` (not mixable without parentheses) | left |
| 4 | `&&` | left |
| 5 | `===` `!==` | left |
| 6 | `<` `<=` `>` `>=` `instanceof` `in` | left |
| 7 | `+` `-` | left |
| 8 | `*` `/` `%` | left |
| 9 | `**` | right |
| 10 | `!` `-` `typeof` `await` `void` `delete` `++` `--` (prefix) | right |
| 11 | `.` `?.` `[ ]` `( )` `++` `--` (postfix) | left |

`yield` and `yield*` take an operand at level 1, so `yield a + b` yields the
sum rather than yielding `a` and then adding.

## Types

CScript is **statically typed, with the annotations optional**. A declaration
that has something to learn its type from takes it and keeps it; there is no
`any`, and no way to opt a variable back out of checking.

```js
const name: string = "cscript";   // annotated
let year = 2026;                  // inferred as number — just as checked
year = "twenty-six";              // error: cannot assign string to 'year'
```

### One-time autocasting

An unannotated declaration takes the type of its initialiser, once, and that
type is fixed for the life of the variable. A declaration with no initialiser —
`let x;` — has nothing to learn from yet, so it **waits**: the first assignment
fixes the type, and every later one has to agree.

```js
let count = 0;        // number from here on
let waiting;          // waiting for its first assignment
waiting = "text";     // string from here on
waiting = 1;          // error: cannot assign number to 'waiting'
```

This is the whole of the rule. A variable never changes type, whether or not it
was annotated — the annotation only moves the decision earlier and writes it
down.

| Type name | Matches |
| --- | --- |
| `number` | all numeric values |
| `bigint` | whole numbers of any size — deliberately not a `number` |
| `string` | string values, of any length |
| `boolean` | `true` and `false` |
| `null` | the `null` literal, and nothing else |
| `undefined` | the `undefined` literal, and a missing value |
| `array` | an array of anything — what `T[]` says when it does not know |
| `object` | an object, including an array and a class instance |
| `Function` | anything callable, signature unknown |
| `unknown` | anything at all — but see below |
| a declared name | an `interface` or a `type`, below |

A type is not only a name. Four ways of building one, as in TypeScript:

| Written | Means |
| --- | --- |
| `T[]` | an array of `T`, and `T[][]` an array of those |
| `(a: number) => string` | a function taking a number and answering a string |
| `A \| B` | one of them — `string \| null` above all |
| `Box<T>` | a declared type with its type arguments |

Spelled as TypeScript spells them, which is what lets `node
--experimental-strip-types` run the same file: the callable type is `Function`
because `function` is a keyword, and a keyword cannot appear in a type. There
is exactly one top type and it is `unknown`; `any` and `value` are refused by
name, with a message saying so.

### `unknown`, and narrowing

`unknown` is the type for what genuinely is not known until run time — a JSON
payload, a comparator's arguments, whatever a caller chose to pass. It is a
**checked** top type, not an escape hatch:

- everything is assignable **to** an `unknown`
- nothing is assignable **from** one, and nothing may be read off one, called,
  indexed or added, until the program has said what it is

`typeof` is how it says so, and the checker follows it:

```js
function describe(thing: unknown): string {
  if (typeof thing === "number") return "number " + String(thing * 2);
  if (typeof thing === "string") return "string of " + String(thing.length);
  return "something else";
}
```

Narrowing works through `&&` and `||`, through a parenthesised group, and
through a guard that leaves — `if (typeof x !== "number") return;` proves what
`x` is for the rest of the block. `Array.isArray(x)` narrows to `array`.

`===` is the one operator an `unknown` needs no narrowing for: asking whether
one equals a number is exactly what narrowing would answer.

### Arrays, functions and unions

An array knows what it holds, and it is inferred from the literal when it is
not written down:

```ts
const xs: number[] = [1, 2, 3];
const names = ["ada", "alan"];   // string[], and just as checked
xs[0] + 1;                       // a number, so this is arithmetic
names[0].toUpperCase();          // a string, so this is a string's method
```

A function type is what makes a callback checkable. Before it, everything
passed as `Function` was the runtime's problem:

```ts
function apply(f: (n: number) => string, x: number): string { return f(x); }
apply((n: number) => "n=" + String(n), 7);   // fine
apply((s: string) => s, 7);                  // error: (string) => string is not (number) => string
```

Parameters are **contravariant** and the result **covariant**, which is the
sound direction: whatever a caller passes has to be acceptable to the function,
and whatever the function answers has to be acceptable to the caller. A
function may be used where one taking more arguments is wanted — the extra ones
are simply not read, which is why `xs.map(x => x)` is legal against a callback
of three parameters.

A function with no return annotation still says what it answers: the type is
taken from its `return` statements, unioned if they differ. A generator and an
async function are exempt, because what calling one answers — a generator, a
promise — is not what its body returns.

A union is one of several types, and `string | null` is the one that earns its
keep: before it, "a string, or nothing" had to be written as a sentinel.
`typeof` narrows a union **in both branches**, and `=== null` / `!== null`
narrow it too:

```ts
function find(haystack: string[], needle: string): string | null { … }

const hit = find(names, "alan");
if (hit !== null) hit.toUpperCase();   // a string here

function widen(x: number | string): string {
  if (typeof x === "number") return String(x + 1);   // a number here
  return x.toUpperCase();                            // a string here
}
```

### Generics

A declaration may be written once for every type it works on. Type arguments
are **inferred** at the call site — `first([1, 2])` says `T` is a number by
being that call:

```ts
function first<T>(items: T[]): T { return items[0]; }
first([1, 2]) + 1;            // a number
first(["a"]).toUpperCase();   // a string

function mapped<T, U>(items: T[], change: (item: T) => U): U[] { … }
mapped(["ada"], (name: string) => name.length);   // number[]

interface Box<T> { value: T; }
const boxed: Box<number> = { value: 41 };
type Pair<A, B> = { left: A; right: B };
```

Inside the declaration a type variable is only itself: nothing else is known
about a `T`, which is what lets the body be checked once for every
instantiation at once. A declaration may take at most four of them, and a call
that leaves one undetermined gets the type the checker could not work out.

### A class's name is a type

A class declares a shape as well as a constructor, and `new Dog()` answers it:

```ts
class Temp {
  celsius: number = 0;
  get label(): string { return String(this.celsius) + "C"; }
  warmer(by: number): Temp { this.celsius += by; return this; }
}

const t = new Temp();
t.warmer(3).label;      // a string — the method says what it answers
t.warmer("lots");       // error: argument 1 is string but this takes number
```

The members are what the class writes down: annotated fields, methods (as
function types), and a getter (as what it answers). A static member belongs to
the class rather than to an instance, and a private `#field` is invisible from
outside, so neither is part of the shape. `extends` carries the members down,
as it does between interfaces.

A class satisfies an interface **structurally**, with nothing declaring that it
does:

```ts
interface HasCelsius { celsius: number; }
function read(x: HasCelsius): number { return x.celsius; }
read(t);   // fine: Temp has a celsius, and it is a number
```

A class's shape is **open**, and an interface's is not: reading a member a
class never declared answers what the checker could not work out rather than an
error, because `this.name = name` in a constructor is how half of them get
their fields. What it *does* declare is checked, and still has to be there for
an instance to satisfy an interface.

### `interface` and `type`

An interface names a shape. It is checked and then erased — nothing it
declares exists at run time, which is also what `node
--experimental-strip-types` does with the same text.

```ts
interface Point {
  x: number;
  y: number;
}

interface Shape extends Point {
  area(): number;    // a method says what it answers
  label?: string;    // optional: it may be missing, but not be the wrong type
}
```

A literal is proved against an interface **where it is given one**, because
that is where its members are still written down:

```ts
const here: Point = { x: 3, y: 4 };   // checked, member by member
const missing: Point = { x: 3 };      // error: Point needs a member 'y'
const extra: Point = { x: 3, y: 4, z: 5 };  // error: Point has no member 'z'
```

An excess member is refused as firmly as a missing one, exactly as TypeScript
refuses one on a fresh literal: a name the interface does not have is nearly
always a misspelling of one it does.

Assignability is **structural**. A `Shape` satisfies a `Point` because it has
everything a `Point` asks for; `extends` copies the members across rather than
recording a link, because presence is all that is ever asked. What does *not*
satisfy an interface is a value already typed `object`: its members are gone
by then, so calling it a `Point` would be a claim nothing checked.

`type` gives a second name to a type that already exists, or writes a shape
inline — the two declare the same thing:

```ts
type Metres = number;
type Pair = { left: number; right: number };
```

Both are **contextual keywords**: `type` and `interface` are ordinary names
everywhere else, and programs use them as such. A type is also declared for
the whole file wherever it is written — a block does not scope one, because
there is nothing of it left at run time to scope.

Three limits, each for the same reason — the checker resolves an annotation
where it reads it, in one pass:

- a type must be **declared before it is named**, except by itself: an
  interface's own name exists before its members are read, so
  `interface Link { next?: Link }` is the way a list is written
- a type is **file-local**. Types do not cross a module boundary yet, so
  `export interface` says so rather than exporting nothing
- a generic takes the number of arguments it declared, and says so when it
  does not: the annotation cannot be resolved at all without them

### Parameters must say

A declaration takes its type from what it is given. A parameter has nothing to
take one from, so it has to say:

```js
function double(n) { return n * 2; }        // error: parameter 'n' needs a type
function double(n: number) { return n * 2; } // fine
```

Without this rule every unannotated parameter would be the escape hatch the
language is built on not having. A destructured parameter is exempt: the
pattern names the parts, and annotating the whole would say less than the
pattern already does.

A rest parameter's annotation describes **one argument**, not the array it is
collected into: `...parts: string` accepts strings and reads as an array
inside the body.

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
let x: any = 1;               // there is no 'any': write 'unknown'
function f(a) {}              // parameter 'a' needs a type
```

### Deliberate gaps

**Comparing against `null` or `undefined` is always allowed**, even when the
other side has a known, different type. Those checks are idiomatic, and without
union types there is no way to write "a string, or null" — so rejecting them
would punish correct code for a hole in the type system.

**An object's properties are dynamic unless an interface names them.**
`Math.PI` and a plain `point.x` are not modelled; reading one answers a type
the checker does not know, and the runtime checks it where it lands. Declaring
an `interface` is how a program opts a shape into being checked.

**A bare `array` says nothing about its elements.** `T[]` does, and a literal
gives one; `array` is what is left when neither did, and a read off it is
checked where it lands.

**A function's return type is inferred when it is not written down**, from the
`return` statements — except for a generator or an async function, where what
the call answers is not what the body returns.

**The type system is deliberately shallow** — a fixed set of types, no
structural or higher-order types, no generics and no unions. That shallowness
is what keeps the checking cheap: a primitive needs one check at a boundary, or
none, where a structural type would need a contract that follows the value
around. Typed Racket measured 10–100× slowdowns from exactly that.

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

**Regular expressions** — `/pattern/flags` literals, with `test` and `exec`,
the `source` `flags` `global` `lastIndex` properties, and the string methods
`match` `search` `replace` `replaceAll` `split`.

Supported: `.`, `\d \D \w \W \s \S`, `[abc]` `[^abc]` `[a-z]`, `^` `$`
`\b` `\B`, `* + ? {n} {n,} {n,m}` and their lazy forms, `(…)` `(?:…)`, `|`,
and the flags `g` `i` `m` `s`. Replacement strings understand `$1`–`$9`, `$&`
and `$$`.

Not supported, each with an error that names it: backreferences, lookahead and
lookbehind, named groups, the `u` and `y` flags, and a function as the
replacement.

A pattern is matched by backtracking, because JavaScript's semantics are
defined in terms of it — leftmost-first alternation, greedy against lazy, and
where a group last matched are all statements about backtracking order. The
cost is that `(a+)+b` can be made to take exponential time, so the matcher
counts its steps and reports rather than hanging.

Two differences from JavaScript. The array `exec` and `match` return holds the
match and its groups but **not** the `index` and `input` properties JavaScript
hangs off it — arrays here are not property bags. And `split` on a pattern with
capture groups does not interleave the captures.

**`Map` and `Set`** — `new Map()`, `new Map([[k, v], …])`, `new Set()`,
`new Set([…])`, with `get` `set` `add` `has` `delete` `clear` `keys` `values`
`entries` `forEach` and a `size` property. Both iterate in insertion order,
survive deletion without disturbing it, work with `for...of` and spread, and
key by **SameValueZero** — `===` except that `NaN` matches itself and `-0`
matches `+0`, which are the two places `===` is surprising. Objects key by
identity.

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

Array and object patterns, with renaming, defaults, a rest element, and
nesting to any depth:

```ts
const { user: { name, tags: [first] } } = data;
const [[a, b], [c]] = pairs;
```

A **parameter** may be a pattern too. It takes a name no source can write and
the pattern is unpacked from it before the body runs, which is exactly what
writing it out by hand would do:

```ts
function greet({ name, greeting = "hi" }) { … }
points.map(({ x, y }) => x * y);
```

Spread works in array literals and call arguments, and over strings.

Not supported, each with an error that says so: **object rest**
(`const { a, ...rest } = o`) and spreading into a built-in method call such as
`xs.push(...ys)` — packing the arguments loses the receiver those need.

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

**`this` is slot 0 of the frame**, for a method and for an ordinary function
alike, which means an arrow function inside either captures it through the
ordinary closure machinery and gets JavaScript's lexical `this` with no rule of
its own. An arrow has no slot 0 of its own, which is the whole of why.

What `this` means is decided by the call, not by how the function was written:
`new F()` puts the object being built there, a call through a property puts the
receiver there, and a bare call leaves it undefined — the same three rules
JavaScript has. Only the top level of a module has no `this` at all, and naming
that at compile time says the same thing as JavaScript's `undefined`, earlier.

**A class method keeps its receiver.** `const f = instance.method; f()` works —
in JavaScript it loses `this`, which is the reason `.bind(this)` exists. This is a
deliberate divergence; see `examples/fixes.cx`.

Two rules are stricter than JavaScript's, both to keep the order above legible:
a subclass constructor must call `super(...)` as its **first** statement, and a
constructor cannot `return` a value.

**Getters and setters** work, including through `super`:

```ts
class Temp {
  celsius = 0;
  get fahrenheit() { return this.celsius * 9 / 5 + 32; }
  set fahrenheit(f: number) { this.celsius = (f - 32) * 5 / 9; }
}
```

An accessor never enters an instance's layout, which is what keeps it out of
the inline caches: a shape hit is always a real field, and only a miss looks
for one. Assigning to a property that has a getter and no setter is an error
rather than a write that silently does nothing.

**Static fields** work too — `static count = 0;` — and are set on the class
where it is declared.

Not supported, each with an error that says so: **private `#fields`**,
**static blocks**, **computed member names**, **`new.target`**, and
**subclassing built-ins**.

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

A specifier is a **relative path with the extension written out**, or a
**`std:` name** for a module of the standard library:

```ts
import { add } from "./math.cx";      // a file, relative to this one
import { range } from "std:iter";     // a module of the standard library
```

Those are the only two forms. There is no package system to resolve a bare
name against, and guessing extensions is how a module system starts needing a
resolver nobody can predict — so a relative specifier says exactly which file
it means, and a `std:` one names something that ships with the language and is
found relative to the binary rather than to the program. See
[../library/README.md](../library/README.md) for what is in it and where it is
looked for.

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

Default exports, `export *`, re-exporting with `export { x } from "..."` and
dynamic `import()` all work. What is not supported is a **bare specifier**:
`import x from "lodash"` has nowhere to look, and says so rather than guessing.
Imported bindings arrive untyped — types do not cross a file boundary yet, so
what a module exports is checked where it is used rather than where it came
from.

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

**Promises.** `new Promise((resolve: Function, reject: Function) => …)`, `Promise.resolve`,
`.reject`, `.all`, `.race`, and `.then` / `.catch` / `.finally`. Settling a
promise *queues* its handlers rather than running them, which is why `.then`
is asynchronous even on an already settled promise, and is what makes ordering
predictable.

**The loop.** Everything synchronous runs first, then the microtask queue is
drained completely, then one timer gets a turn, and so on until both are
empty. `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` and
`queueMicrotask` are the way in. An interval is re-armed after its callback
returns rather than on a fixed grid, so a slow callback cannot queue up behind
itself — and it may stop itself, which is where most intervals are stopped.

A rejection nothing ever listened to is reported and exits non-zero, as it
does in Node — a promise that failed with no one watching is a bug, and the
alternative is a program that silently does half its work.

**Top-level `await`** works in any file, including one that another file
imports. A file that awaits runs as an async body on a fiber, and the loader
drives the event loop until it settles before starting whatever imported it —
so the ordering guarantee the loader is built on still holds.

**Generators** — `function*`, `yield`, `yield*` — run on the same fibers async
functions do. What differs is only who resumes them: the event loop when a
promise settles, or `next()` when someone pulls. `for...of` over a generator
is pull-driven, so an endless one is fine as long as the loop leaves.

**Async generators** — `async function*` — suspend for two different reasons:
an `await`, which the event loop resumes, and a `yield`, which whoever called
`next` resumes. Their `next()` answers with a promise, because the body may
await any number of times before it reaches the yield that has the value.

**`for await`** drives an async generator, awaits each element of any sync
iterable, and pulls from any object that offers `Symbol.asyncIterator`. Which
of the shapes runs is decided per iteration by looking at the iterable, so the
sync path is unchanged.

## Not implemented yet

Each of these produces an error that names it, except where noted:

- `yield*` inside a larger expression (it works as a statement of its own, and
  a delegate's return value is not available)
- `Date`'s locale formats — `toLocaleString`, `toLocaleDateString` — and
  `Date.parse` of anything but ISO, which is implementation-defined in
  JavaScript too
- `arguments` (a rest parameter does the same job) and subclassing built-ins
- Bare import specifiers like `import x from "lodash"`: there is no package
  system to resolve one against
- Sparse arrays and holes, which are why engines need a second array
  representation
- Unicode-correct string indexing: strings are indexed by byte, which is
  correct for ASCII
- Property order puts integer-like keys in insertion order, where JavaScript
  puts them first and in ascending order — no error, a different order
- `with` and `eval`, which have no plans

[JAVASCRIPT.md](JAVASCRIPT.md) carries the same list beside what *is* supported
and what differs on purpose.
