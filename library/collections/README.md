# `std:collections`

The four structures a Map and an array do not cover.

```ts
import { Deque, PriorityQueue, Counter, DefaultMap } from "std:collections";

const queue = new Deque([1, 2]);        queue.pushFront(0);
const next = new PriorityQueue();        next.push(3);
const counts = new Counter("mississippi");
const groups = new DefaultMap(() => []); groups.get("a").push(1);
```

All four are iterable, so `for...of` and `[...]` work on each.

## Specification

### `Deque` — a double-ended queue

`new Deque(initial?)`

| Member | Signature | Behaviour |
| --- | --- | --- |
| `size` | getter | how many it holds |
| `isEmpty` | getter | whether that is zero |
| `pushBack` / `pushFront` | `(value)` | adds at either end, O(1) amortised |
| `popBack` / `popFront` | `()` | removes and answers, or `undefined` when empty |
| `peekBack` / `peekFront` | `()` | answers without removing |
| `at` | `(index)` | by position from the front, `undefined` out of range |
| `clear` | `()` | empties it |
| `toArray` | `()` | front to back |

### `PriorityQueue` — a binary heap

`new PriorityQueue(compare?, initial?)` — `compare` defaults to ascending
numbers, so a max-heap is `new PriorityQueue((a, b) => b - a)`.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `size` / `isEmpty` | getters | how many, and whether none |
| `push` | `(value)` | O(log n) |
| `pop` | `()` | the smallest by the comparator, or `undefined` |
| `peek` | `()` | the smallest, without removing |

Iterating **drains** it, which is what makes it a sort.

### `Counter` — how many times each thing was seen

`new Counter(initial?)` — counts every element of the iterable.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `size` | getter | how many distinct keys |
| `total` | getter | the sum of the counts |
| `add` | `(key, howMany = 1)` | adds; reaching zero removes the key |
| `get` | `(key)` | the count, **0** for something never seen |
| `has` / `delete` | `(key)` | as a Map |
| `mostCommon` | `(howMany = -1)` | `[key, count]` pairs, most frequent first |
| `keys` | `()` | the keys, in insertion order |

### `DefaultMap` — a Map that builds its missing values

`new DefaultMap(build, initial?)` — `build` is called with the key when it
declares a parameter, and with nothing when it does not.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `size` | getter | how many keys |
| `get` | `(key)` | the value, **building and storing one** if absent |
| `has` | `(key)` | whether it is there — deliberately does *not* build |
| `set` / `delete` | `(key, value)` / `(key)` | as a Map |
| `keys` / `values` | `()` | as a Map |

## Implementation

**`Deque` is a ring buffer, not an array with `shift`.** `shift` moves every
remaining element down one, so a queue built on it costs O(n) per item taken
and O(n²) to drain — invisible at ten elements and fatal at a hundred
thousand. Here both ends are an index update. The buffer doubles when full, so
a push is O(1) amortised: every element is copied at most once per doubling.

**`PriorityQueue` is a heap because the alternatives are worse.** Sorting after
each insertion is O(n log n) every time; scanning for the minimum is O(n) per
read. A heap is O(log n) for both. It cannot be walked in order without taking
it apart, which is why iterating drains it.

**`Counter.get` answers 0 rather than `undefined`** — a count is a number, and
`counter.get(x) + 1` should not be `NaN` for the first one.

**`Counter` and `DefaultMap` hold a Map, not an object**, so a key can be a
number, a boolean or an object rather than only a string.

**`DefaultMap.has` does not build.** Asking whether something is there should
not be what puts it there.

## Source

[`collections.cx`](collections.cx). `Deque.#grow` and `PriorityQueue.#swap`
are private; everything else is reachable from the four classes.
