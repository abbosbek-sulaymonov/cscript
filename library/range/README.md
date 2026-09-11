# `std:range`

An interval as a value.

```ts
import { range, inclusive } from "std:range";

const valid = range(1, 101);
if (!valid.contains(input)) input = valid.clamp(input);
```

Not to be confused with [`iter.range`](../iter), which *produces* numbers. This
*describes* an interval: it can be asked whether it contains something, clamped
against, intersected with another, and compared — and it can also be iterated,
which is where the two meet.

The distinction matters because the questions are different. A loop wants
numbers one at a time; a validator wants to hold "1 to 100" as one thing and
ask about it repeatedly. Passing `iter.range(1, 101)` around cannot do the
second: a generator is used up once it has been read, and this can be walked
twice.

## Specification

### Building one

| Export | Signature | Holds |
| --- | --- | --- |
| `range` | `(start, end)` | `start` up to but **not including** `end` |
| `inclusive` | `(start, last)` | both ends |
| `Range.ofLength` | `(start, length)` | that many from `start` |
| `Range.covering` | `(values)` | the smallest range holding everything given |

### Reading one

| Member | Is |
| --- | --- |
| `start` | the first value |
| `end` | **exclusive** — the first value not in it |
| `last` | the last value in it, which is what a reader means by "end" |
| `length` | how many whole numbers it holds; 0 for a backwards range |
| `isEmpty` | whether it holds nothing |
| `toArray` / `[Symbol.iterator]` | the whole numbers in it |

### Asking

| Member | Signature | Answers |
| --- | --- | --- |
| `contains` | `(value)` | whether the value is in it |
| `containsRange` | `(other)` | whether every value of `other` is |
| `overlaps` | `(other)` | whether they share any value |
| `clamp` | `(value)` | the value held inside; throws on an empty range |
| `equals` | `(other)` | whether they describe the same interval |

### Combining

| Member | Signature | Answers |
| --- | --- | --- |
| `intersect` | `(other)` | the overlap, empty when there is none |
| `union` | `(other)` | the smallest range holding both |
| `shifted` | `(by)` | moved, keeping its length |
| `grown` | `(by)` | wider at both ends; negative narrows it |

## Implementation

**The end is exclusive by default**, matching every slice, every substring and
`iter.range` itself. `inclusive(1, 10)` exists because "1 to 10" in prose
usually means both ends, and the mismatch between the two readings is a
reliable source of off-by-one. Both are held as a half-open pair whatever they
were built from, so every operation has one case rather than two.

**`last` is there because `end` surprises people.** Both are offered rather
than one, since half the uses want each.

**`clamp` throws on an empty range** rather than answering a number that is not
in it. Every other operation treats empty as a well-behaved value: it is inside
anything, overlaps nothing, and unions to the other side.

**`union` may hold what sits between.** Two ranges that do not touch have no
single interval covering only them, so the answer covers the gap as well —
`overlaps` is how to find out whether that happened.

**Iterating walks whole numbers**, which is why this is iterable at all: a
range of reals has no next value. A fractional start walks in ones from there,
because that is the only stride the range itself implies.

## Source

[`range.cx`](range.cx). Nothing private; `range` and `inclusive` are the
spellings the rest of the library uses.
