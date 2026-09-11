# `std:time`

Durations, and measuring how long something took.

```ts
import { minutes, formatDuration, Stopwatch, measure } from "std:time";

const timeout = minutes(5);
const { value, elapsed } = measure(() => work());
console.log("took " + formatDuration(elapsed));
```

A `Date` is an instant: the millisecond it happened. A duration is a *length*
of time, and JavaScript has no type for one — so every program writes
`5 * 60 * 1000` and every reader has to work out what it meant.

## Specification

### The units

`MILLISECOND`, `SECOND`, `MINUTE`, `HOUR`, `DAY`, `WEEK` as constants, and
`milliseconds`, `seconds`, `minutes`, `hours`, `days` as functions of a count.

| Export | Signature | Answers |
| --- | --- | --- |
| `toSeconds` / `toMinutes` / `toHours` / `toDays` | `(duration)` | the duration in that unit, fractions included |
| `parts` | `(duration)` | `{ negative, days, hours, minutes, seconds, milliseconds }`, none overflowing into the next |

### Text

| Export | Signature | Answers |
| --- | --- | --- |
| `formatDuration` | `(duration, most = 2)` | `"1h 2m"` — at most that many units, zeros skipped |
| `parseDuration` | `(text)` | `"1h30m"`, `"2.5s"`, `"500ms"` as a number; **NaN** for anything it cannot read |

### Instants

| Export | Signature | Answers |
| --- | --- | --- |
| `now` | `()` | the current instant |
| `formatInstant` | `(instant)` | ISO 8601 — sortable as text, unambiguous about the zone |
| `formatClock` | `(instant, withMilliseconds = false)` | `"01:02:03"` |
| `formatDate` | `(instant)` | `"1971-02-05"` |
| `startOfDay` | `(instant)` | the midnight it falls after |
| `sameDay` | `(first, second)` | whether they fall on the same day |

### Measuring

| Member | Signature | Behaviour |
| --- | --- | --- |
| `new Stopwatch` | `(startNow = true)` | starts unless told not to |
| `start` | `()` | restarts it |
| `elapsed` | getter | milliseconds since the start, frozen once stopped, **0** before it began |
| `isRunning` | getter | started and not stopped |
| `lap` | `()` | the time since the last lap |
| `stop` | `()` | the final elapsed time; stopping twice reads the same |
| `measure` | `(body)` | `{ value, elapsed }` — the whole pattern in one call |

## Implementation

**A duration is a number of milliseconds, not an opaque type.** Arithmetic on
durations is the whole point of having them, and a class would need `plus` and
`times` to get back what `+` and `*` already do.

**`formatDuration` shows two units by default** because more is noise: `"1h 2m"`
is what someone reading a log wants, not `"1h 2m 3s 456ms"`. Zero is `"0ms"`
rather than the empty string.

**`parseDuration` answers NaN rather than throwing**, so a caller can decide
whether unreadable text is a mistake or a reason to use a default. It reads the
number and the unit letters by hand, because a regular expression covering
`"1h30m"`, `"2.5s"` and `"1h 30m"` would be as long as the loop.

**A Stopwatch that was never started measures nothing.** Its start is
`undefined` rather than 0, because 0 is a real instant — the first of January
1970 — so the obvious spelling would have reported fifty-odd years of elapsed
time.

**Everything is UTC.** A local time needs a timezone database, which is a
project of its own, and the getters CScript's `Date` offers are the UTC ones.

## Source

[`time.cx`](time.cx). `isDigitAt` and `pad` are private.
