# `std:stats`

Summarising a list of numbers.

```ts
import { mean, median, stdev, quantile, describe } from "std:stats";

const sample = [2, 4, 4, 4, 5, 5, 7, 9];
console.log(mean(sample), median(sample));      // 5 4.5
console.log(quantile(sample, 0.25));            // 4
```

Every function takes anything `for...of` accepts and answers a number.

## Specification

### Totals and middles

| Export | Signature | Answers |
| --- | --- | --- |
| `sum` | `(values)` | the total; **0** for an empty sequence |
| `product` | `(values)` | the product; **1** for an empty one |
| `mean` | `(values)` | the arithmetic mean |
| `median` | `(values)` | the middle, or the mean of the middle two |
| `mode` | `(values)` | an **array** of every value tied for most frequent |

### Extremes

| Export | Signature | Answers |
| --- | --- | --- |
| `min` / `max` | `(values)` | the lowest or highest |
| `spread` | `(values)` | `max - min` |

### Dispersion

| Export | Signature | Answers |
| --- | --- | --- |
| `variance` | `(values)` | the **sample** variance, divided by n−1; needs two values |
| `populationVariance` | `(values)` | divided by n |
| `stdev` | `(values)` | the square root of `variance` |
| `populationStdev` | `(values)` | the square root of `populationVariance` |

### Positions

| Export | Signature | Answers |
| --- | --- | --- |
| `quantile` | `(values, fraction)` | the value at 0…1, interpolating between neighbours |
| `percentile` | `(values, percent)` | the same, at 0…100 |

### Single numbers

| Export | Signature | Answers |
| --- | --- | --- |
| `clamp` | `(value, low, high)` | the value held inside the range |
| `lerp` | `(from, to, at)` | linear interpolation; outside 0…1 it extrapolates |
| `normalize` | `(value, low, high)` | where the value sits in the range, as 0…1 |
| `round` | `(value, places = 0)` | rounded to that many decimal places |

### Several at once

| Export | Signature | Answers |
| --- | --- | --- |
| `describe` | `(values)` | `{ count, min, max, mean, median, stdev }` |
| `cumulativeSum` | `(values)` | a running total, one per input |
| `zScores` | `(values)` | how many standard deviations each is from the mean |

## Implementation

**An empty sequence is refused, not answered with NaN.** The mean of nothing is
a question with no answer, and NaN travelling through three more calculations
before showing up is how a wrong number gets printed with confidence. The
private `numbers` helper checks the type of every element too, and names the
offender — the difference between finding the bad row and searching for it.
`sum` and `product` are the exceptions: an empty total is 0 and an empty
product is 1, which are the identities and not guesses.

**`variance` divides by n−1 by default.** That is Bessel's correction, and it
is the right default because a sample is almost always what one has: dividing
by n underestimates the variance of the population it was drawn from.
`populationVariance` is there for when the values really are everything.

**`median` and `quantile` sort a copy.** A function that summarises its input
should not rearrange it.

**`quantile` interpolates linearly between the two neighbours**, which is the
method every spreadsheet uses — so a number from here matches one from there.

**`lerp` extrapolates outside 0…1** rather than clamping, which is what makes
it usable for an animation that overshoots on purpose.

**The arithmetic is annotated `number` throughout.** Not decoration: a
fully-typed function is one the compiler can lower to machine code with no type
guards at all, and these are the functions most likely to be called in a loop.

## Source

[`stats.cx`](stats.cx). `numbers` is private — the one place the input is
checked, so everything else can assume it.
