# `std:func`

Building a function out of other functions.

```ts
import { pipe, memoize, once } from "std:func";

const slug = pipe((s: string) => s.trim(), (s: string) => s.toLowerCase());
const fib = memoize((n: number) => n < 2 ? n : fib(n - 1) + fib(n - 2));
const init = once(() => connect());
```

## Specification

### Constants

| Export | Signature | Answers |
| --- | --- | --- |
| `identity` | `(value)` | the value, unchanged |
| `constant` | `(value)` | a function answering that value, whatever it is called with |
| `noop` | `()` | `undefined` |

### Composing

| Export | Signature | Answers |
| --- | --- | --- |
| `pipe` | `(...steps)` | a function running the steps **left to right** |
| `compose` | `(...steps)` | the same, **right to left** |
| `partial` | `(fn, ...bound)` | `fn` with those leading arguments fixed |
| `curry2` | `(fn)` | `(a) => (b) => fn(a, b)` |
| `curry3` | `(fn)` | `(a) => (b) => (c) => fn(a, b, c)` |
| `flip` | `(fn)` | `fn` with its first two arguments swapped |
| `negate` | `(test)` | a predicate answering the opposite |

### Remembering

| Export | Signature | Behaviour |
| --- | --- | --- |
| `once` | `(fn)` | runs the body at most once; answers that first value ever after |
| `memoize` | `(fn, keyOf?)` | remembers what each argument list answered |

### Odds and ends

| Export | Signature | Behaviour |
| --- | --- | --- |
| `tap` | `(fn)` | runs `fn` for its effect, answers the value unchanged |
| `times` | `(count, fn?)` | an array of `fn(i)`, or of the indices when `fn` is absent |

## Implementation

**Arity is why this file looks the way it does.** CScript checks it, so a
composed function is called with exactly the arguments it declared: `pipe`
passes one value along, and `memoize` passes through the arguments it was
handed rather than adding a context nobody asked for.

**`pipe` is left to right because that is the order the steps are written in.**
`compose` is the same thing the other way round, for readers who learned it
from mathematics. Both take one value — a pipeline of multi-argument steps has
no single sensible threading rule.

**Currying is spelled out at two and three arguments rather than generalised.**
A general `curry` has to guess when it has enough arguments, and guessing wrong
gives a function that silently returns another function instead of an answer.

**`once` keeps a flag rather than testing the result**, so a body that answered
`undefined` is still not run twice.

**`memoize` keys on JSON.** That works for numbers, strings and plain
structures and not for anything with identity: two different objects with the
same fields are one key. Pass `keyOf` where that is wrong. It is only worth
using on a function that is expensive *and* pure — a memoized function that
reads anything outside its arguments answers the first reading for ever.

## Source

[`func.cx`](func.cx). Nothing private; every function here is one of its
exports.
