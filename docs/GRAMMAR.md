# CScript grammar and semantics — v0.2.0

CScript's syntax is a **subset of JavaScript's**: every CScript program is a
valid JavaScript program, and `make test-node` enforces that by running the
examples under Node and diffing the output.

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

varDeclaration = ( "let" | "const" ) IDENTIFIER ( "=" expression )? ";" ;
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
- `-0` prints as `0`, matching `String(-0)` in JavaScript
- `%` takes the sign of the left operand: `-7 % 3` is `-1`

### Strings

Immutable and interned — two strings with the same contents are the same
object, so equality is a pointer comparison.

Escapes: `\n` `\t` `\r` `\0` `\\` `\'` `\"`. An unrecognised escape keeps the
character as written. Template literals are not implemented.

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

## Not implemented yet

Each of these produces an error that names it:

- Functions, `return`, closures, arrow functions
- Objects and object literals, arrays, indexing
- `switch`, `do`/`while`, `break`, `continue`, labels
- `for...of`, `for...in`
- Template literals, ternary `?:`, spread, destructuring
- `try`/`catch`/`throw`, classes, modules, `async`/`await`
