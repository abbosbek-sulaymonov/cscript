# `std:assert`

Checking what a program believes, and saying so when it is wrong.

```ts
import { equal, deepEqual, throws } from "std:assert";

equal(sum([1, 2, 3]), 6);
deepEqual(parse("a=1"), { a: 1 });
throws(() => divide(1, 0), "cannot divide by zero");
```

Every failure throws an `Error` whose `name` is `"AssertionError"`, so a runner
can tell an assertion from a bug in the code under test.

## Specification

### Truth

| Export | Signature | Fails when |
| --- | --- | --- |
| `ok` | `(value, message?)` | `value` is falsy |
| `notOk` | `(value, message?)` | `value` is truthy |
| `unreachable` | `(message?)` | always — for a branch that should not run |

### Equality

| Export | Signature | Fails when |
| --- | --- | --- |
| `equal` | `(actual, expected, message?)` | not the same value |
| `notEqual` | `(actual, expected, message?)` | the same value |
| `deepEqual` | `(actual, expected, message?)` | not structurally equal |
| `notDeepEqual` | `(actual, expected, message?)` | structurally equal |
| `closeTo` | `(actual, expected, tolerance = 1e-9, message?)` | further apart than the tolerance |

### Containment

| Export | Signature | Fails when |
| --- | --- | --- |
| `includes` | `(haystack, needle, message?)` | the string or sequence does not contain it |
| `hasLength` | `(value, length, message?)` | `value.length` is not that |

### Throwing

| Export | Signature | Answers |
| --- | --- | --- |
| `throws` | `(body, expected?, message?)` | what was thrown, so a caller can assert about it |
| `doesNotThrow` | `(body, message?)` | what the body returned |

`expected` narrows what counts: a **string** is matched against the thrown
error's message, a **function** is asked and must answer true. Anything else is
ignored, so `throws(fn)` accepts any throw.

### Reading a failure

| Export | Signature | Answers |
| --- | --- | --- |
| `messageOf` | `(thrown)` | the message of anything that was thrown, including a plain string |
| `describe` | `(value)` | a value as text, with strings quoted |
| `isDeepEqual` | `(a, b)` | whether two values are structurally equal, as a boolean |

`isDeepEqual` is the predicate behind `deepEqual`, exported for the cases where
a boolean is wanted rather than a throw.

## Implementation

**Why it throws rather than returning false.** An assertion whose result is
ignored is worse than no assertion, because it reads like a check that passed.

**`describe` rather than `String()`.** A string has to come back quoted, or
`equal("1", 1)` reports `1 !== 1` and reads like a bug in the assertion rather
than in the program.

**Equality is SameValueZero.** `NaN` equals itself — which is what makes an
array of computed numbers comparable at all — and `-0` equals `0`. The second
is a deliberate difference from Node's `deepStrictEqual`, which distinguishes
them: a sign of zero that no arithmetic can observe is not worth failing a test
over.

**`deepEqual` compares primitives, arrays and plain objects.** It cannot
compare a Map or a Set structurally, because CScript has no way to tell one
from a plain object at runtime: reading `.size` off something that is not a Map
is a runtime error rather than `undefined`, so there is no safe test to make.
Compare `[...map]` instead, which is an array of pairs.

**What `throws` cannot see.** A *runtime* error — a property that is not there,
a number added to an object — is not catchable in CScript, so `throws` sees a
`throw` the program made and nothing else. That is the same line the language
draws everywhere: a decision the program took, against a mistake it made. A
body that trips a runtime error takes the process down, assertion or not.

## Source

[`assert.cx`](assert.cx). `fail` and `same` are private: the first builds the
AssertionError, the second is the equality every comparison here rests on.
