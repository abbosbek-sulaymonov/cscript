# CScript grammar and semantics — v0.1.0

What the language accepts *today*. Anything not listed here is not implemented
yet; see the roadmap in [ARCHITECTURE.md](ARCHITECTURE.md).

## Grammar

Written in EBNF. `*` is zero or more, `?` is optional, `|` is alternation.

```ebnf
program        = statement* EOF ;

statement      = printStmt
               | expressionStmt ;

printStmt      = "print" expression ";" ;
expressionStmt = expression ";" ;

expression     = logicalOr ;

logicalOr      = logicalAnd ( "||" logicalAnd )* ;
logicalAnd     = equality   ( "&&" equality )* ;
equality       = comparison ( ( "==" | "!=" | "===" | "!==" ) comparison )* ;
comparison     = term       ( ( "<" | "<=" | ">" | ">=" ) term )* ;
term           = factor     ( ( "+" | "-" ) factor )* ;
factor         = unary      ( ( "*" | "/" | "%" ) unary )* ;

unary          = ( "!" | "-" | "typeof" ) unary
               | primary ;

primary        = NUMBER | STRING
               | "true" | "false" | "null" | "undefined"
               | "(" expression ")" ;
```

## Operator precedence

Loosest binding first. Every binary operator is left-associative; unary
operators are right-associative.

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `\|\|` | left |
| 2 | `&&` | left |
| 3 | `==` `!=` `===` `!==` | left |
| 4 | `<` `<=` `>` `>=` | left |
| 5 | `+` `-` | left |
| 6 | `*` `/` `%` | left |
| 7 | `!` `-` `typeof` | right |

## Types

Five of JavaScript's primitives exist. `symbol` and `bigint` do not.

| Type | Literals | `typeof` |
| --- | --- | --- |
| number | `42`, `3.14`, `1e3`, `1.5e-2`, `0x1F` | `"number"` |
| string | `"double"`, `'single'` | `"string"` |
| boolean | `true`, `false` | `"boolean"` |
| null | `null` | `"object"` |
| undefined | `undefined` | `"undefined"` |

`typeof null` returning `"object"` is a bug in the original JavaScript that can
never be fixed without breaking the web. CScript reproduces it deliberately.

### Numbers

All numbers are IEEE 754 doubles. There is no integer type.

- Integral values print without a decimal point: `10 / 2` prints `5`
- `1 / 0` is `Infinity`, `-1 / 0` is `-Infinity`, `0 / 0` is `NaN`
- `-0` prints as `0`, matching `String(-0)` in JavaScript
- `%` takes the sign of the left operand: `-7 % 3` is `-1`

### Strings

Immutable and interned — two strings with the same contents are the same object.

Escapes: `\n` `\t` `\r` `\0` `\\` `\'` `\"`. An unrecognised escape keeps the
character as written, as in JavaScript. Template literals are not implemented.

## Semantics

### Truthiness

Six values are falsy: `false`, `null`, `undefined`, `0`, `NaN`, `""`.
Everything else is truthy.

### Equality

`===` compares type first — values of different types are never strictly equal.
`NaN === NaN` is `false`, per IEEE 754.

`==` coerces before comparing:

- `null == undefined` is `true`, and neither is loosely equal to anything else
- otherwise both sides are coerced to numbers, then compared

```js
1 == "1"           // true
1 === "1"          // false
0 == false         // true
0 === false        // false
null == undefined  // true
null === undefined // false
```

### `+`

If **either** operand is a string, both are converted to strings and
concatenated. Otherwise both must be numbers and they are added. Any other
combination is a runtime error.

```js
1 + 2          // 3
"a" + "b"      // "ab"
"n = " + 42    // "n = 42"
true + " x"    // "true x"
true + 1       // runtime error: cannot add boolean and number
```

Other arithmetic operators (`-` `*` `/` `%`) require both operands to be
numbers and do not coerce. This is stricter than JavaScript, which would give
`NaN`; an explicit error is more useful than a value that silently poisons
everything downstream.

### `&&` and `||`

Both short-circuit, and both evaluate to **one of their operands** rather than
to a boolean:

- `a && b` → `a` if `a` is falsy, otherwise `b`
- `a || b` → `a` if `a` is truthy, otherwise `b`

```js
"value" || "unused"   // "value"
null || "fallback"    // "fallback"
"first" && "second"   // "second"
0 && "never"          // 0
```

## Comments

```js
// to end of line

/* block comments,
   which may span lines */
```

Block comments do not nest.

## Statements

Semicolons are **required**. Automatic semicolon insertion is not implemented.

`print` is a statement, not a function. It exists only because this milestone
has no functions to call, and it is replaced by `console.log` in milestone 6.

## Not implemented yet

Using any of these is a syntax error with a message saying so:

- Variables and assignment (`let`, `const`, `var`)
- Control flow (`if`, `else`, `while`, `for`)
- Blocks and scoping
- Functions, calls, `return`, closures
- Objects, arrays, property access, indexing
- Template literals, `+=` and friends, `++`/`--`, ternary `?:`
- Unary `+`
- `try`/`catch`, modules, classes
