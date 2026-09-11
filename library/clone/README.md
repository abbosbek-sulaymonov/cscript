# `std:clone`

Copying a value rather than sharing it.

```ts
import { deep, shallow, withFields } from "std:clone";

const copy = deep(settings);              // independent all the way down
const next = withFields(user, { age: 37 }); // a copy with one field changed
```

Assignment shares: `const b = a` gives two names for one object, and a write
through either is visible through both. That is usually what is wanted and
occasionally the source of a bug nothing points at — a "copy" of a
configuration that turns out to be the configuration.

## Specification

| Export | Signature | Answers |
| --- | --- | --- |
| `shallow` | `(value)` | the top level copied, everything below it **shared** |
| `deep` | `(value)` | copied all the way down |
| `deepReport` | `(value)` | `{ value, shared }` — the copy, and the path of everything it could only share |
| `withFields` | `(value, changes)` | a shallow copy with those keys replaced |
| `withoutFields` | `(value, keys)` | a copy without those keys |
| `pickFields` | `(value, keys)` | only those keys, and only the ones present |

**What gets copied:** arrays, plain objects, Maps, Sets, Dates, and every
primitive. A Map's **keys** are copied too — a key that is an object is a
different key once copied, which is the point of a deep copy and would be a
surprise if only the values were.

**What stays shared:** a class instance, a function, a promise, a generator.
None can be taken apart from outside, and a copy of an instance's public half
would be a different object wearing the same name.

## Implementation

**The difference between the two depths matters more than it looks.** A shallow
copy of `{ a: { b: 1 } }` shares the inner object, so `copy.a.b = 2` changes
the original. That is the bug this module is usually reached for after.

**Cycles are preserved rather than refused.** The map from original to copy is
what makes `a.self = a` come back as a copy whose `self` is that same copy, and
it is also what stops the walk from recursing for ever. Two references to one
object stay one object in the copy, for the same reason.

**`deepReport` exists because the alternative is a silent half-copy.** A deep
clone that quietly shares a class instance is the failure mode this whole
project is written against, so the paths where the copy still meets the
original can be asked for.

**Asking the kind first is what makes the walk safe.** Walking a Map with
`Object.keys` is a runtime error rather than an empty answer, so every branch
here goes through [`std:kind`](../kind).

**`withFields` is one call on purpose.** "Change one field" is otherwise
written as a mutation because that was shorter.

## Source

[`clone.cx`](clone.cx). `copyInto` and `copyReporting` are private: the same
walk, one of them keeping a record.
