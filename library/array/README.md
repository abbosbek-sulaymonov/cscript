# `std:array`

The array operations the built-ins do not have.

```ts
import { splice, sortBy, binarySearch, dedupe } from "std:array";

const without = splice(items, 2, 1);        // CScript arrays have no splice
const ordered = sortBy(people, (p: object) => p.name);
```

CScript's arrays answer `push`, `pop`, `shift`, `unshift`, `slice`, `concat`,
`sort`, `map`, `filter`, `reduce` and the rest. What is missing is the
positional work — `splice` above all, which the runtime has no equivalent of —
and the searching and ordering everyone rewrites.

Everything here is **eager and non-mutating**: it answers a new array and
leaves its argument alone. For lazy work over a sequence use
[`std:iter`](../iter); for counting occurrences use `Counter` in
[`std:collections`](../collections).

## Specification

### Positions

| Export | Signature | Answers |
| --- | --- | --- |
| `splice` | `(items, start, remove = -1, ...insert)` | a new array with that range replaced; a negative `start` counts from the end |
| `insertAt` | `(items, index, value)` | with the value put in at that index |
| `removeAt` | `(items, index)` | without the element at that index |
| `removeValue` | `(items, value)` | without the **first** element equal to it |
| `swapped` | `(items, first, second)` | with those two positions exchanged |
| `rotated` | `(items, by)` | moved towards the front, wrapping; negative rotates back |
| `reversed` | `(items)` | back to front |

### Ordering

| Export | Signature | Answers |
| --- | --- | --- |
| `sortBy` | `(items, keyOf)` | sorted by what `keyOf` answers — strings included |
| `sortWith` | `(items, compare)` | sorted by a comparator |
| `dedupe` | `(items, keyOf?)` | the first of each, by value or by key |

### Searching

| Export | Signature | Answers |
| --- | --- | --- |
| `binarySearch` | `(items, target, comparison?)` | the index in a **sorted** array, or -1; O(log n) |
| `insertionIndex` | `(items, target, comparison?)` | where it would go to keep the array sorted |

### Shapes

| Export | Signature | Answers |
| --- | --- | --- |
| `flattenDeep` | `(items)` | every level opened out |
| `first` / `last` | `(items)` | the ends, or `undefined` |
| `isEmpty` | `(items)` | whether there is nothing |
| `filled` | `(length, value)` | that many copies |
| `padded` | `(items, length, value)` | padded on the right; a longer array is left alone |

## Implementation

**`splice` is the reason the module exists.** The runtime has no `splice` at
all, so inserting or removing in the middle of an array had to be written out
every time. This is the whole of JavaScript's behaviour, answering a new array
rather than editing in place.

**Non-mutating throughout.** A function that summarises or reorders should not
also rearrange what it was given, because the caller is usually still holding
it — which is why `reversed` and `sortBy` exist beside the built-in `reverse`
and `sort`, both of which edit in place.

**Ordering goes through [`std:cmp`](../cmp).** In CScript `<` is numeric, so
comparing two strings with it is a runtime error; `sortBy` would otherwise work
on numbers and crash on names.

**`insertionIndex` exists because `binarySearch` cannot answer it.** A search
that answers -1 has thrown away the one thing a caller inserting into a sorted
array needs.

**`binarySearch` is wrong rather than slow on an unsorted array**, which is why
it is named for its method instead of being offered as a faster `indexOf`.

**`flattenDeep` goes all the way down**, unlike the built-in `flat()` with its
default depth of one and unlike `iter.flatten`, which stops at one on purpose.

## Source

[`array.cx`](array.cx). Nothing private; `splice` is what most of the
positional functions are written in terms of.
