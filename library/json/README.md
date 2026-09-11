# `std:json`

JSON that can fail without ending the program.

```ts
import { parse, stringifyStable } from "std:json";

const config = parse(text).unwrapOr({});
const written = stringifyStable(config);   // sorted keys, two-space indent
```

The built-in `JSON.parse` reports bad input as a **runtime** error, and a
runtime error is not catchable in CScript. So a program that parses anything it
did not write — a config file, an argument, a response — could not survive a
malformed one. The parser here is written in CScript, so a failure is a
[`Result`](../result) with a position in it rather than the end of the process.

## Specification

### Parsing

| Export | Signature | Answers |
| --- | --- | --- |
| `parse` | `(text)` | `Result` of the value, or of `"at 12: expected a value"` |
| `parseOr` | `(text, fallback)` | the value, or the fallback |

### Printing

| Export | Signature | Answers |
| --- | --- | --- |
| `stringify` | `(value, indent = 0, stable = false)` | JSON text; `indent` above zero pretty-prints |
| `pretty` | `(value, indent = 2)` | indented, keys in their own order |
| `stringifyStable` | `(value, indent = 2)` | indented, keys **sorted** |

What it writes that the built-in does not: a **Map** keeps its entries and a
**Set** becomes an array. A Date becomes its ISO string. A function, a promise,
a generator or a class throws rather than being quietly dropped.

## Implementation

**It is JSON, not JavaScript.** No comments, no trailing commas, no unquoted
keys, no single quotes — and each is refused by name, because every one of them
is a thing someone will have written by hand:

```
at 0: JSON has no comments
at 3: JSON has no trailing commas
at 1: a key must be a quoted string
at 0: JSON strings use double quotes
```

**The position is a character offset**, which is what a caller needs to point
at the problem: a line and column can be worked out from the text and the
offset, and cannot be recovered from a line and column alone.

**The printer is written out rather than wrapping `JSON.stringify`**, because
the two things it needs — indentation, and keys in a stable order — are what
make JSON output diffable, and the built-in offers neither. Insertion order is
not part of what JSON means, so output that depends on it produces a diff for a
change nobody made.

**`\u` escapes are decoded to UTF-8**, which is what a CScript string holds and
what the source lexer produces for the same character. A surrogate pair is two
escapes meaning one code point, and is joined rather than written as two
unpaired halves. Fixing this module's `\u` handling turned up the same bug in
the built-in parser, which had been replacing every escape above 127 with a
literal `?`.

**`NaN` and `Infinity` are written as `null`**, which is what every other
implementation does — neither has a JSON spelling. A key whose value is
`undefined` is left out rather than written as null, which is also what the
built-in does.

## A note on the built-in printer

`JSON.stringify(new Map([["k", 1]]))` answers `undefined` in CScript. Node
answers `"{}"` — a Map has no own enumerable properties, so it writes an empty
object and loses everything in it. Refusing is the better of the two answers,
and this module's `stringify` is the third: it writes the entries.

## Source

[`json.cx`](json.cx). The parser is a recursive descent over a small state
object; `utf8`, `hexValue` and `quote` are private.
