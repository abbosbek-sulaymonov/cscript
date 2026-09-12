# `std:text`

Comparing text, and putting it on a terminal.

```ts
import { closest, diffLines, formatDiff, table, bold } from "std:text";

closest("comit", commands);                 // "commit"
console.log(formatDiff(diffLines(a, b)));
console.log(table(rows, { head: ["file", "lines"], align: ["left", "right"] }));
```

[`std:strings`](../strings) is the operations on one string. This is the ones
involving two — how far apart they are, and what changed — and the ones
involving a terminal.

## Specification

### Distance

| Export | Signature | Answers |
| --- | --- | --- |
| `levenshtein` | `(a, b)` | the edit distance |
| `similarity` | `(a, b)` | 1 for identical, 0 for nothing in common |
| `closest` | `(text, candidates, threshold = 0.6)` | the nearest, or `undefined` |

### Difference

| Export | Signature | Answers |
| --- | --- | --- |
| `diffLines` | `(before, after)` | `{ kind, text }` per line — `"same"`, `"added"`, `"removed"` |
| `formatDiff` | `(changes, context?)` | the `-`/`+` form; `context: 0` drops unchanged lines |

### Names

| Export | Signature | Answers |
| --- | --- | --- |
| `slugify` | `(text, separator = "-")` | lower case, words joined, nothing else kept |
| `naturalCompare` | `(a, b)` | `item2` before `item10` |

### A terminal

| Export | Signature | Answers |
| --- | --- | --- |
| `style` | `(text, ...names)` | wrapped in escapes; `bold`, `dim`, `red`, `green`, `yellow`, `blue` are shorthands |
| `stripStyle` / `width` | `(text)` | without the escapes, and how wide that is |
| `setColour` / `isColoured` | `(on)` / `()` | whether colour is written at all |
| `table` | `(rows, { head, align, separator })` | aligned columns |
| `columns` | `(items, { width, gap })` | down then across, the way `ls` lays them out |

## Implementation

**`levenshtein` keeps two rows, not a table.** The algorithm only looks at the
previous row, and a full table for two thousand-character strings is a million
entries for nothing.

**`closest` is the "did you mean" behind a good error message.** An unknown
command or field is far more useful with a suggestion attached, and the
threshold is a *ratio* because a distance of 3 means something different for a
word and for a sentence.

**`diffLines` finds the longest common subsequence**, which is what makes the
answer minimal: a line-by-line comparison calls everything after an inserted
line changed, which is the diff everybody has seen and nobody can read. The
table is O(n × m), so this is for files rather than for logs.

**`slugify` drops bytes above 127 rather than transliterating.** Turning "é"
into "e" needs a table per language — in some, "ö" transliterates as "oe" — and
a wrong transliteration is worse than an honest omission.

**Widths are measured with the escapes stripped**, which is why a coloured
column lines up with a plain one. A styled string's `length` counts the escape
bytes, and that is why the hand-written version never aligns.

**Colour is a flag the program sets.** The runtime cannot tell whether its
output is a terminal, so this remembers the decision rather than guessing it.

**The escape character is built from its code**, not written literally: a
literal ESC byte in a source file is invisible in every editor and survives
exactly one careless copy.

## Source

[`text.cx`](text.cx). `isDigitAt` is private; `ESCAPE` and `CODES` are the
terminal's two constants.
