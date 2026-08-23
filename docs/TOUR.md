# A tour of CScript

Enough of the language to judge it, in one page. The complete syntax and
semantics are in [GRAMMAR.md](GRAMMAR.md); how any of it works is in
[ARCHITECTURE.md](ARCHITECTURE.md).

Full reference: **[docs/GRAMMAR.md](GRAMMAR.md)**.

```js
// Variables — let and const only, both block-scoped.
const limit = 10;
let total = 0;

for (let i = 1; i <= limit; i++) {
  total += i * i;
}
console.log("sum of squares:", total);   // 385

// Blocks scope, and inner names shadow outer ones.
let name = "outer";
{
  let name = "inner";
  console.log(name);   // inner
}
console.log(name);     // outer

// && and || return an operand, not a boolean, and short-circuit.
console.log(null || "fallback");   // fallback
console.log(0 && "never");         // 0

// Conversions are explicit.
console.log(Number("42") + 1);     // 43
console.log(String(42) + "!");     // 42!

// Classes, with fields, inheritance and `this`.
class Shape {
  sides = 0;
  constructor(name) { this.name = name; }
  describe() { return `${this.name}: ${this.sides} sides`; }
}

class Square extends Shape {
  sides = 4;
  constructor() { super("square"); }
}

console.log(new Square().describe());        // square: 4 sides
console.log(new Square() instanceof Shape);  // true
```

| Group | Operators |
| --- | --- |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `**=` |
| Arithmetic | `+` `-` `*` `/` `%` `**` `++` `--` |
| Comparison | `<` `<=` `>` `>=` |
| Equality | `===` `!==` |
| Logical | `&&` `\|\|` `!` |
| Other | `typeof` `instanceof` `?:` `...` `.` `[ ]` `( )` `=>` |

Built-ins: `console`, `Math`, `Object`, `Array`, `Number`, `JSON`, plus
`String` / `Boolean` / `Error` / `parseInt` / `parseFloat` / `isNaN` /
`isFinite` / `NaN` / `Infinity`. Arrays carry 21 methods and strings 21 more.
All of it is read-only — `Math.PI = 3` is an error, not a surprise later.

### Errors point at the problem

```
$ cscript -e 'console.log(1 +);'
<argv>:1: error: expected an expression
      1 | console.log(1 +);
        |                ^
```

The parser recovers at statement boundaries, so one bad line does not hide the
rest of the file.

---

## How it works

Full detail: **[docs/ARCHITECTURE.md](ARCHITECTURE.md)**.

```
source ──▶ Lexer ──▶ Parser ──▶ Compiler ──▶ VM ──▶ output
             │         │           │          │
          tokens      AST       bytecode    values
```

Four stages, each with one job, its own header, and a debug flag that dumps
exactly what it produced. `make trace` turns all four on at once.

Design decisions worth stating up front:

**A bytecode VM, not a tree walker.** Flat instruction arrays stay
cache-friendly, recursion depth is bounded by the value stack rather than the C
stack, and it is the shape every production JavaScript engine takes.

**An AST is kept anyway.** A single-pass compiler would be smaller, but the AST
is the only structure a formatter, linter or optimiser can work on. It lives
only between parsing and code generation, in an arena freed in one call.

**Scopes are resolved at compile time.** A local variable compiles to a stack
slot index, so reading a loop counter is an array index — not the hash lookup a
naive interpreter would do on every iteration. Globals still hash: `bench/`
measures the same program both ways and locals come out about 30% faster.

**A garbage collector from day one.** Retrofitting GC means touching every
allocation site. Wiring it in while there was one object type was nearly free —
and it paid off immediately when milestone 2 added two more.

---
