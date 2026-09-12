# `std:bytes`

A sequence of bytes, and the numbers hidden inside one.

```ts
import { fromString, toHex, readUint32BE } from "std:bytes";

const header = fromString(text);
const length = readUint32BE(header, 4);
```

CScript has no typed arrays — no `Uint8Array`, no `ArrayBuffer`, no `DataView`.
So a byte sequence here is a plain **array of numbers, each 0 to 255**, and
this module is what makes that a usable type.

## The cost, stated plainly

Every byte occupies a full `Value` — eight bytes of memory rather than one —
and every read is an array index rather than a load. Fine for a hash of a few
kilobytes, a base64 of a line, or a binary header; **not** a buffer for a
video. A native byte array is the obvious future optimisation, and the
interface here is deliberately the one such a type would have.

The other half: **a CScript string is already bytes.** `text.charCodeAt(i)` is
the byte at `i`, so `fromString` and `toString` are exact and lossless in both
directions, whatever the bytes are.

## Specification

### Making one

| Export | Signature | Answers |
| --- | --- | --- |
| `fromString` / `toString` | `(text)` / `(bytes)` | the bytes of a string, and back |
| `of` | `(...values)` | a sequence from its bytes |
| `alloc` | `(length, fill = 0)` | that many bytes |
| `concat` | `(...parts)` | one sequence from several |
| `isBytes` | `(value)` | whether every element really is a byte |

### Comparing

| Export | Signature | Answers |
| --- | --- | --- |
| `equals` | `(a, b)` | whether they are the same |
| `compare` | `(a, b)` | -1, 0 or 1, like `memcmp` |
| `equalsConstantTime` | `(a, b)` | the same, looking at every byte whatever happens |

### Slicing and searching

`slice`, `indexOf`, `includes`, `startsWith`, `endsWith` — the array
operations, on sequences rather than elements.

### Numbers at an offset

| Export | Reads or writes |
| --- | --- |
| `readUint8` / `writeUint8` | one byte |
| `readUint16BE` / `readUint16LE` and the writers | two, either way round |
| `readUint32BE` / `readUint32LE` and the writers | four |
| `readInt32BE` / `readInt32LE` | the same four, as signed |

## Implementation

**A byte that is not a whole number from 0 to 255 is a mistake worth stopping
on.** Writing its low eight bits instead would be a wrong answer that looks
like a right one.

**Reading past the end throws** rather than answering a sequence full of
`undefined` — which in arithmetic is NaN, and would travel a long way before
anyone noticed.

**`toString` builds in blocks.** A string built by repeated concatenation is
quadratic, and a megabyte of bytes is a real thing to convert.

**`equalsConstantTime` is the right shape, not a guarantee.** `equals` stops at
the first difference, so how long it takes says where that difference was —
enough to recover a secret one byte at a time. Nothing else in CScript is
written with timing in mind, so treat this as a good habit.

**The read and write helpers exist because endianness is where binary formats
go wrong.** Working it out with multiplication at every call site is how one of
the five call sites ends up the other way round.

## Node cannot check the non-ASCII half

`tests/cases/stdlib/bytes.cx` has its expected output produced by CScript. A
Node string is UTF-16, so `"é".charCodeAt(0)` is 233 there and 195 here — which
is the very difference this module exists to work with.

## Source

[`bytes.cx`](bytes.cx). `check` and `bounded` are private: the two validations
every other function goes through.
