# `std:bits`

The bitwise operations the language does not have.

```ts
import { and, or, shiftLeft, popcount, Bitset } from "std:bits";

const masked = and(flags, 0xff);
const seen = new Bitset(1000000);
```

CScript has no `&`, `|`, `^`, `~`, `<<` or `>>`: writing one is a syntax error
that suggests `&&`. So this is not a convenience layer over operators — it
**is** the bitwise arithmetic, done on doubles.

That works because every value here is an unsigned 32-bit integer and a double
holds every integer up to 2^53 exactly, so a 32-bit result never loses a bit.
The results were checked against Node's own operators across every combination
of a spread of inputs, rotations and arithmetic shifts included.

## Specification

### Keeping to 32 bits

| Export | Signature | Answers |
| --- | --- | --- |
| `wrap` | `(value)` | the 32 bits a register would hold: truncated, then wrapped |
| `toSigned` | `(value)` | the same bits read as signed — 4294967295 is -1 |
| `fromSigned` | `(value)` | a signed number as its 32 bits |

### The operators

| Export | Signature | Is |
| --- | --- | --- |
| `and` / `or` / `xor` | `(a, b)` | `&` / `\|` / `^` |
| `not` | `(value)` | `~` |
| `shiftLeft` | `(value, by)` | `<<` |
| `shiftRight` | `(value, by)` | `>>>` — zeros come in at the top |
| `shiftRightSigned` | `(value, by)` | `>>` — the sign bit is copied |
| `rotateLeft` / `rotateRight` | `(value, by)` | bits that leave one end arrive at the other |

### One bit at a time

| Export | Signature | Answers |
| --- | --- | --- |
| `testBit` | `(value, index)` | whether it is set; **false** out of range |
| `setBit` / `clearBit` / `toggleBit` | `(value, index)` | the value with that bit changed |

### Counting

| Export | Signature | Answers |
| --- | --- | --- |
| `popcount` | `(value)` | how many bits are set |
| `leadingZeros` / `trailingZeros` | `(value)` | how many, 32 for zero |
| `highestBit` | `(value)` | the position of the top set bit, or -1 |

### Powers of two

| Export | Signature | Answers |
| --- | --- | --- |
| `isPowerOfTwo` | `(value)` | whether exactly one bit is set |
| `nextPowerOfTwo` | `(value)` | the smallest power of two at least that big |

### Reading and writing

| Export | Signature | Answers |
| --- | --- | --- |
| `toBinary` / `toHex` | `(value, width = 0)` | text, zero-padded to `width` |
| `fromBinary` | `(text)` | the number; throws on anything but 0 and 1 |

### `Bitset`

`new Bitset(size = 0)` — a bit per index, over as many 32-bit words as needed,
growing as indices are set.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `set` / `clear` | `(index)` | one bit; answers the Bitset, so calls chain |
| `test` | `(index)` | whether it is set; false out of range |
| `size` | getter | the highest index plus one |
| `count` | getter | how many are set |
| `toArray` | `()` | the indices that are set, in order |
| `[Symbol.iterator]` | `()` | the same, as a generator |

## Implementation

**`and`, `or` and `xor` walk 32 bits, low to high.** Thirty-two iterations of
three operations, which is slower than an instruction and is the price of the
operators not existing. One loop serves all three, differing by the line that
combines the two bits.

**The shifts are multiplication and division by powers of two**, with a modulo
to keep the width. Bits shifted off the top are gone, as in hardware.
`shiftRightSigned` divides the *signed* view, which is what copies the sign.

**`nextPowerOfTwo` saturates rather than wrapping.** Anything past 2^31 answers
2^31, because a size that silently became zero is the worst answer available.

**`Bitset` is one bit per element** where a Set of numbers would be many bytes,
which is what makes a sieve or a visited-set over a million indices reasonable.
`count` works word by word — thirty-two bits per iteration instead of one — and
iterating is a generator so a large set need not build an array to be read.

## Source

[`bits.cx`](bits.cx). `pairwise` is private: the bit-by-bit loop behind the
three binary operators.
