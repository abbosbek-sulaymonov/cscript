# `std:random`

Randomness you can repeat.

```ts
import { Random, seeded, random } from "std:random";

const dice = seeded(42);
console.log(dice.int(1, 6), dice.int(1, 6));   // the same two numbers every run
console.log(random.next());                     // seeded from the clock
```

`Math.random` cannot be seeded, which makes it useless for the two things
randomness is most often needed for: a test that must fail the same way twice,
and a simulation someone else has to be able to reproduce.

## Specification

### `Random`

`new Random(seed = 0)` — the seed is floored and made positive, because the
state has to be a whole number inside the modulus for the recurrence to close.

| Member | Signature | Answers |
| --- | --- | --- |
| `next` | `()` | a float, 0 inclusive to 1 exclusive — the same contract as `Math.random` |
| `int` | `(low, high)` | a whole number, **both ends inclusive** |
| `float` | `(low = 0, high = 1)` | a float in the range |
| `bool` | `(chance = 0.5)` | true with that probability |
| `pick` | `(items)` | one element; throws on an empty list |
| `shuffle` | `(items)` | a **new** array, Fisher–Yates |
| `sample` | `(items, howMany)` | that many distinct elements, without replacement |
| `weighted` | `(items, weights)` | one item, with probability proportional to its weight |
| `gaussian` | `(centre = 0, deviation = 1)` | a normally distributed number, by Box–Muller |
| `stream` | `()` | an **unbounded** generator of `next()` values |

### Module level

| Export | Is |
| --- | --- |
| `random` | a `Random` seeded from `Date.now()`, for when reproducibility is not the point |
| `seeded(seed)` | `new Random(seed)`, spelled as an intention |

## Implementation

**A linear congruential generator**, with Numerical Recipes' constants:
`state = (state * 1664525 + 1013904223) % 2^32`. Chosen because every step
stays inside the 53 bits a double holds exactly, so the sequence is identical
on every machine and every run. That property is the whole point of the module.

**Not cryptographic.** The next value follows from the last, and anyone who
sees one output can compute the rest. Nothing here should protect anything.

**`int` includes both ends**, because "a number between 1 and 6" means that to
everyone who is not writing an array index.

**`shuffle` copies.** Shuffling in place would be faster and would also mean
that shuffling a list of results reorders whatever else is holding it — a bug
the caller cannot see coming.

**`weighted` normalises the weights itself.** Requiring a caller to make them
add to 1 is requiring them to redo it every time the list changes.

**`gaussian` throws away Box–Muller's second value.** Caching it would make the
sequence depend on how many gaussians were asked for rather than on how many
steps were taken, which is exactly the reproducibility this class exists for.

**`random` is a `Random`, not a wrapper over `Math.random`**, so switching a
program to a fixed seed changes one line.

## Source

[`random.cx`](random.cx). `#step` is the private one-line recurrence everything
else is built on.
