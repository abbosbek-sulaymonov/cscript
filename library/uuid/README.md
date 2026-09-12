# `std:uuid`

Identifiers that do not need a central authority.

```ts
import { v4, v7, timeOf } from "std:uuid";

const id = v7();            // time-ordered: sorts by when it was made
const random = v4();
```

## Not cryptographically random

CScript has no secure source of randomness — no `crypto.getRandomValues`, and
no way to build one in the language — so these are built on `Math.random` or on
a seeded generator from [`std:random`](../random). Fine for a database key, a
request id or a filename. **Not** fine for a session token, a password reset
link, or anything an attacker benefits from guessing.

## Specification

| Export | Signature | Answers |
| --- | --- | --- |
| `v4` | `(source?)` | sixteen random bytes — the one everybody means by "a UUID" |
| `v7` | `(source?, at?)` | the millisecond first, then randomness |
| `format` | `(bytes)` | sixteen bytes as the canonical 8-4-4-4-12 |
| `isValid` | `(text)` | whether it is a UUID at all; case is ignored |
| `versionOf` | `(text)` | 4, 7, 0 for the nil one, or **-1** |
| `timeOf` | `(text)` | the instant a **v7** was made, or -1 |
| `nil` | `()` | all zeros, for a column that cannot be null |

`source` answers a float in [0, 1) — the same contract as `Math.random` and
`std:random`'s `next`.

## Implementation

**Prefer v7 for anything stored.** Its first 48 bits are the millisecond, so a
column of them sorts into the order they were made — which is what makes it a
far better database key than v4, whose randomness scatters every insert across
the index. The id also carries *when*, recoverable with `timeOf`.

**The random source and the clock are both injectable**, which is the only way
a test of something that makes identifiers can be a golden test at all.

**The version and variant bits are what make it a UUID** rather than sixteen
arbitrary bytes: the 13th hex digit is the version and the 17th is 8, 9, a or
b. `stamp` sets both.

**v7's timestamp is written by division rather than shifting**, since 48 bits is
past what [`std:bits`](../bits)' 32-bit operations hold.

## Source

[`uuid.cx`](uuid.cx). `randomBytes`, `stamp` and `isHexDigit` are private.
