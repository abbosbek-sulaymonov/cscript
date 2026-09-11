# `std:cmp`

Comparators, and the one the language cannot write.

```ts
import { compareStrings, by, then } from "std:cmp";

names.sort(compareStrings);
people.sort(then(by((p: any) => p.age), by((p: any) => p.name)));
```

`<` and `>` in CScript are **numeric**: comparing two strings with them is a
runtime error, not a lexicographic answer. So while `["b", "a"].sort()` works —
the built-in sort compares strings internally — a *comparator* over strings
cannot be written in CScript at all without going through character codes.
`compareStrings` is that, and everything here which orders anything other than
numbers rests on it.

## Specification

### The primitives

| Export | Signature | Orders |
| --- | --- | --- |
| `compareNumbers` | `(a, b)` | numerically |
| `compareStrings` | `(a, b)` | lexicographically by byte — the same order the built-in `sort()` uses |
| `compareStringsInsensitive` | `(a, b)` | case-folded, ties broken by the case-sensitive order |
| `compareBooleans` | `(a, b)` | false before true |
| `compare` | `(a, b)` | whichever of those fits; anything else by its text |

### Building one

| Export | Signature | Answers |
| --- | --- | --- |
| `by` | `(keyOf, order?)` | orders by what `keyOf` answers |
| `byDescending` | `(keyOf, order?)` | the same, reversed |
| `reversed` | `(order)` | an order turned around |
| `then` | `(...orders)` | the first order with an opinion decides |
| `nullsFirst` | `(order?)` | `null` and `undefined` before everything |
| `nullsLast` | `(order?)` | and after |

### Using one

| Export | Signature | Answers |
| --- | --- | --- |
| `minBy` / `maxBy` | `(items, order?)` | the extreme without sorting — O(n); `undefined` for an empty sequence |
| `isSorted` | `(items, order?)` | whether it is already ordered |

## Implementation

**`compareNumbers` is not `a - b`.** The subtraction overflows to Infinity for
large values and answers NaN when either side is NaN, and a comparator
answering NaN sorts unpredictably rather than failing.

**`compareStrings` walks character codes** because `<` on two strings is a
runtime error here. A prefix comes first when one string is a prefix of the
other, which is the rule every other language uses. It agrees with the
built-in `sort()` with no comparator, so the two can be mixed.

**`then` is a fold, not a chain of `||`.** A comparator answering 0 is falsy,
so the shorter spelling would be wrong for a tie in the middle of the list.

**`nullsFirst` can only move `null`.** The runtime sorts every `undefined` to
the end *before any comparator runs* — the specification's rule, and Node's —
so a comparator never sees one and cannot place it. `nullsFirst` is therefore
about `null`, and `[3, undefined, 1]` sorts to `[1, 3, undefined]` whatever it
is given.

**`compareBooleans` puts false first**, which is the order that makes
`sortBy(tasks, isDone)` show the unfinished work at the top.

## Source

[`cmp.cx`](cmp.cx). Nothing private.
