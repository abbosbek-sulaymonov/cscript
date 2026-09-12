# `std:csv`

Comma-separated values, including the parts that are not commas.

```ts
import { parseObjects, formatObjects } from "std:csv";

const rows = parseObjects(text).unwrapOr([]);
const out = formatObjects(rows, { columns: ["name", "age"] });
```

CSV looks like `text.split(",")` for about an hour — until a field contains a
comma, or a quote, or a newline, and the split version produces rows of the
wrong length that nobody notices until the numbers are wrong.

## Specification

| Export | Signature | Answers |
| --- | --- | --- |
| `parse` | `(text, options?)` | `Result` of rows, each an array of strings |
| `parseObjects` | `(text, options?)` | `Result` of objects, the first row being the names |
| `format` | `(rows, options?)` | CSV text with a trailing newline |
| `formatObjects` | `(records, options?)` | the same, with a header row |

`options` is `{ delimiter, trim, skipEmpty, quoteAll, columns }`.

### What it follows

RFC 4180: a field may be quoted; a quoted field may contain the delimiter, a
newline, or a doubled quote meaning one. Line endings may be `\n`, `\r\n` or a
bare `\r` — a file written on a classic Mac is still a file.

## Implementation

**The delimiter is a parameter**, because tab-separated files are the same
format with a different character and having two modules for that would be
silly.

**Trimming applies to unquoted fields only.** A quoted field means exactly what
is inside the quotes — that is what quoting it was for.

**A short row is refused, not padded.** In `parseObjects`, a row whose length
does not match the header means the file is not what the caller thinks it is,
and filling the gap with an empty string hides that.

**Writing quotes only what has to be quoted.** A file where every field is
quoted is harder for a person to read and no easier for a program — `quoteAll`
is there for the formats that demand it.

**`columns` is usually worth giving.** Taking the header from the first record's
keys works, and the order then depends on how that object happened to be built.

**What it refuses is narrow** — an unterminated quoted field, and a row that does
not match its header. Everything else that looks like a mistake is something a
real file does.

## Source

[`csv.cx`](csv.cx). `finish` and `quote` are private.
