# `std:log`

Output a program can turn down, and a machine can read.

```ts
import { Logger, WARN } from "std:log";

const log = new Logger({ level: WARN, format: "json" });
log.warn("slow request", { ms: 1200, path: "/search" });
// {"time":"…","level":"warn","message":"slow request","ms":1200,"path":"/search"}
```

`console.log` is two things at once: what a program prints because printing it
*is* the job, and what it prints so someone can work out what it did. The second
wants a level, a timestamp and the fields saying which request this line is
about — and it wants to be turned off without being deleted.

## Specification

### Levels

`DEBUG` (10), `INFO` (20), `WARN` (30), `ERROR` (40), `SILENT` (100).

| Export | Signature | Answers |
| --- | --- | --- |
| `levelNamed` | `(name)` | the level for `"debug"`, `"warn"`, `"off"`…; **-1** for a name that is not one |
| `levelName` | `(level)` | `"info"` |

### `Logger`

`new Logger({ level, format, fields, write, clock, time })` — all optional.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `debug` / `info` / `warn` / `error` | `(message, fields?)` | one line, when the level allows |
| `log` | `(level, message, fields?)` | the same, with the level given |
| `enabled` | `(level)` | whether such a line would be written at all |
| `setLevel` | `(level)` | turns the volume up or down |
| `with` | `(fields)` | a logger carrying those fields, sharing everything else |
| `level` | getter | the current level |

### Ready-made

`base` is a logger at `INFO`, text, to the console, and `debug`/`info`/`warn`/
`error` are its methods as plain functions — so `log.info("…")` works with no
setup, and a program that needs more replaces it in one line.

### The two formats

```
2023-11-14T22:13:20.000Z warn  slow ms=1200 message="two words"
{"time":"2023-11-14T22:13:20.000Z","level":"warn","message":"slow","ms":1200}
```

## Implementation

**The clock is injectable, and that is not a testing convenience bolted on.** A
logger whose output cannot be reproduced cannot be checked by a golden test,
and everything in this library is.

**`write` takes a finished line**, which makes a Logger usable against a file, a
`StringWriter` from [`std:io`](../io), or a test's array without knowing about
any of them.

**`with` is the shape most programs need**: one logger per request or per file,
carrying its fields without being told again. It copies rather than mutating, so
a child cannot change what its parent logs.

**The level is checked before the fields are merged**, so a `debug` line in a
hot loop costs a comparison when debug is off. `enabled` is there for when
building the *message* is itself expensive.

**Text quotes a value only when it contains a space.** Quoting everything makes
the common line harder to read; quoting nothing makes `message=hello world`
ambiguous.

**JSON is written by hand rather than through [`std:json`](../json)** to keep
the field order fixed — time, level, message, then the fields. A log file that
reorders its own columns is one nobody can diff.

**Deliberately absent:** rotation, files, syslog, sampling. A `write` function
is the seam where all of those belong, and each is a decision about deployment
rather than about logging.

## Source

[`log.cx`](log.cx). `renderText`, `renderJson` and `quote` are private.
