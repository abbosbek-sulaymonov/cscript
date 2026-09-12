# `std:decimal`

Money, and everything else that must add up exactly.

```ts
import { Decimal } from "std:decimal";

Decimal.parse("0.1").add(Decimal.parse("0.2")).toString();   // "0.3"
0.1 + 0.2;                                                   // 0.30000000000000004
```

A Decimal is an **integer number of units and a scale**: 12.34 is 1234 units at
scale 2. Every operation is integer arithmetic, so addition, subtraction and
multiplication are exact, and division says what to do about the remainder
rather than guessing.

## Specification

### Making one

| Export | Signature | Answers |
| --- | --- | --- |
| `Decimal.parse` | `(text)` | exact; the **scale comes from the text**, so `"1.500"` keeps three places |
| `Decimal.of` | `(value, scale = 2)` | from a number or a string |
| `Decimal.zero` | `(scale = 2)` | nothing, at that scale |
| `new Decimal` | `(units, scale)` | the raw form, rarely what a caller wants |

**Prefer `parse`.** `Decimal.of(0.1)` has already lost the exactness this type
is for — it can only recover what the double actually holds.

### Arithmetic

| Member | Signature | Behaviour |
| --- | --- | --- |
| `add` / `subtract` | `(other)` | both sides brought to the wider scale |
| `multiply` | `(other)` | the scales **add**, as they do on paper |
| `divide` | `(other, scale?, mode?)` | must be told how many places to keep |
| `percent` | `(amount, mode?)` | at the receiver's own scale |
| `negate` / `abs` | `()` | the sign flipped, and the sign dropped |

`other` may be a Decimal, a string, or a number — and a **whole** number
converts at scale 0, so `price.multiply(2)` answers two places rather than
four.

### Rounding

`round(scale, mode)` with `"half-up"` (the default), `"half-even"` (banker's),
`"down"`, `"up"`, `"floor"`, `"ceil"`. `rescale(scale)` moves to a wider scale
exactly, and refuses a narrower one.

### Comparing and reading

`compare`, `equals`, `isZero`, `isNegative`, `toString`, `toNumber`, `units`,
`scale`.

## Implementation

**`0.1 + 0.2` is not `0.3` in binary floating point.** That is arithmetic
rather than a bug, and it is fatal for anything counted in cents: a thousand
additions of 0.1 drift far enough to show in a total, and a comparison against
an expected value fails for reasons nobody can see in the printout.

**The bound is 2^53**, the largest integer a double holds exactly — about 90
trillion at scale 2. Past it, every operation **throws** rather than quietly
losing the last digits, which is the behaviour that makes the type worth
using.

**Rounding is written out rather than deferred to `Math.round`**, which is
half-up for positives and half-*down* for negatives: `-0.5` becomes `-0` and
`0.5` becomes `1`. That is not symmetric and is not what any accountant means.

**Division is told its scale** because most divisions have no exact answer, and
a default would be a guess that is wrong in exactly the cases this type exists
for. It computes two extra places so the rounding decision has digits to look
at.

**`toString` writes every place the scale claims** — `"1.50"`, not `"1.5"`,
because a price at scale 2 is written with two places.

**`toNumber` is lossy by definition.** For a chart or something that only takes
numbers; never for arithmetic that has to add up.

## Source

[`decimal.cx`](decimal.cx). `guard`, `align`, `asDecimal` and `roundUnits` are
private.
