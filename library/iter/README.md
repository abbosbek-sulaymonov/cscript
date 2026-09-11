# `std:iter`

Sequences, one element at a time.

```ts
import { range, map, filter, take, count } from "std:iter";

console.log([...map(range(1, 6), (n: number) => n * n)]);   // [1, 4, 9, 16, 25]
console.log([...take(count(0, 5), 3)]);                     // [0, 5, 10]
```

Everything takes anything `for...of` accepts — an array, a string, a Map, a
Set, another generator — and everything that produces a sequence is a
generator, so nothing is built that nobody asks for.

## Specification

### Making a sequence

| Export | Signature | Produces |
| --- | --- | --- |
| `range` | `(start, stop?, step = 1)` | `range(4)` is 0…3; `range(1, 4)` is 1…3; `range(3, 0, -1)` counts down |
| `count` | `(start = 0, step = 1)` | every number from `start`, **without end** |
| `repeat` | `(value, times = -1)` | `value` that many times; below zero, without end |
| `cycle` | `(iterable)` | the sequence, then again, without end |

### Reshaping one

| Export | Signature | Produces |
| --- | --- | --- |
| `map` | `(iterable, transform)` | each element transformed |
| `filter` | `(iterable, keep)` | the elements that pass |
| `take` | `(iterable, howMany)` | at most that many from the front |
| `drop` | `(iterable, howMany)` | everything after the first that many |
| `takeWhile` | `(iterable, keep)` | the leading run that passes |
| `dropWhile` | `(iterable, skip)` | everything from the first that does not |
| `zip` | `(first, second)` | `[a, b]` pairs, stopping with the shorter |
| `enumerate` | `(iterable, start = 0)` | `[index, element]` pairs |
| `chain` | `(...iterables)` | each in turn |
| `flatten` | `(iterable)` | arrays opened out, **one level** |
| `unique` | `(iterable, keyOf?)` | the first of each, by value or by key |
| `chunk` | `(iterable, size)` | arrays of that size; the last may be short |
| `pairwise` | `(iterable)` | overlapping pairs: `[1,2,3]` gives `[1,2]`, `[2,3]` |

`map` and `filter` pass the index as a second argument **only to a callback
that declared two parameters** — see below.

### Reading one

| Export | Signature | Answers |
| --- | --- | --- |
| `toArray` | `(iterable)` | an array — the same array if it was one already |
| `reduce` | `(iterable, combine, initial?)` | the accumulated value; throws on an empty sequence with no initial |
| `some` | `(iterable, test)` | whether any element passes |
| `every` | `(iterable, test)` | whether all do |
| `find` | `(iterable, test)` | the first that passes, or `undefined` |
| `findIndex` | `(iterable, test)` | its position, or `-1` |
| `length` | `(iterable)` | how many there are — walks the sequence |
| `groupBy` | `(iterable, keyOf)` | a **Map** from key to array of elements |
| `partition` | `(iterable, keep)` | `[passed, rest]`, in one walk |

## Implementation

**Nothing runs until the sequence is walked.** A function returning a generator
has done no work when it returns — and walking it twice walks the source twice,
so a generator, unlike an array, is used up once it has been read. That is the
single most important thing to know before composing these.

**An unbounded sequence is the caller's problem to bound.** `count()` and
`repeat(x)` never end, which is what makes them useful: `take(count(), 5)`
finishes, `[...count()]` does not.

**Callbacks are called with as many arguments as they declared.** CScript
checks arity, so handing an index to a one-parameter arrow is an error rather
than an ignored extra. `callIndexed` reads `callback.length` and calls with one
argument or two — exactly what the built-in array methods do, so `map` here and
`map` there behave the same way for the same callback.

**`zip` pulls lazily.** It was written to collect both sides first, which made
`zip(range(3), cycle(xs))` run out of memory: collecting walks the endless side
for ever. It now steps both through `asStream`, a private generator that turns
any iterable into something answering `next()` — the only handle CScript gives
on "one element at a time from a source I did not create", since an array does
not answer `next` and a generator does.

**`flatten` stops at one level.** Flattening to the bottom needs a rule for
what counts as a sequence, and a string is one — so `flatten(["ab"])` would be
a surprise.

**`chunk`'s last chunk is short rather than padded.** Padding invents elements;
a caller who wants them can say so with `chain`.

**`groupBy` answers a Map, not an object.** A key can then be a number, a
boolean or an object — which is most of what one groups by.

**`partition` walks once.** `filter` twice would be two walks, and the second
walk of a generator is empty.

## Source

[`iter.cx`](iter.cx). Private: `callIndexed` (arity adaptation) and `asStream`
(anything iterable as a generator).
