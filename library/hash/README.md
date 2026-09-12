# `std:hash`

Turning bytes into a number, fast.

```ts
import { fnv1a, crc32, murmur3, bucket } from "std:hash";

const shard = bucket(userId, 16);
const checksum = crc32(fileText);
```

For bucketing, deduplicating, caching, sharding and checksums.

## Not for security

Every function here is trivially reversible and trivially collided on purpose.
None is a substitute for SHA-256, and **CScript has no cryptographic primitives
at all** — not even a source of secure randomness, so one cannot be built in
the language either. This is said plainly because the mistake is common and
quiet: a password stored as an FNV hash looks hashed.

## Specification

| Export | Signature | Is |
| --- | --- | --- |
| `fnv1a` | `(input)` | FNV-1a, 32-bit — the default: two operations per byte and a good spread |
| `djb2` | `(input)` | Bernstein's, kept because it is the one people recognise |
| `crc32` | `(input)` | the checksum in zip, gzip and PNG |
| `murmur3` | `(input, seed = 0)` | better distributed on longer keys; a seed makes two tables disagree |
| `toHexString` | `(value)` | the hash as eight hex digits |
| `bucket` | `(input, count)` | which of `count` buckets it falls in |
| `fingerprint` | `(input)` | a short stable identifier — `murmur3` in hex |

`input` is a string **or** a byte array, and the two agree, since a CScript
string is bytes.

## Implementation

**The values match every other implementation.** A checksum that only agrees
with itself would be a hash. All four were checked against reference
implementations — `crc32` against zlib's — across a spread of inputs.

**32-bit multiplication has to be done in halves.** `a * b` for two 32-bit
numbers reaches 2^64, well past the 2^53 a double holds exactly, so the low
half would be wrong in a way no test on small inputs would show. Splitting one
side into 16-bit halves keeps every product under 2^48.

**The CRC table is built once, on first use.** 256 entries of eight shifts each
is worth doing once rather than per call.

**Every answer is an unsigned 32-bit number**, which is what makes `bucket`'s
modulo safe: a signed hash is negative half the time, and `hash % count` then
indexes backwards off the front of an array.

**The arithmetic goes through [`std:bits`](../bits)**, which is where the 32-bit
operations live in a language with no bitwise operators.

## Source

[`hash.cx`](hash.cx). `asBytes`, `buildCrcTable` and `multiply` are private.
