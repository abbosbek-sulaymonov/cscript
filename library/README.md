# `library/` — the standard library

Twenty-eight modules, written in CScript, imported by name:

```ts
import { range, zip } from "std:iter";
import { mean, stdev } from "std:stats";

console.log(mean([...range(1, 11)]));
```

A `std:` specifier is not a path. It names a module in the library that ships
with the language, so a script anywhere on disk means the same file by it —
everything else must be a relative path, so that reading an import tells you
which file it means.

**A module is a directory.** `library/iter/iter.cx` is the source and
`library/iter/README.md` is its specification — what every export takes and
answers, and the reasoning behind how it is written. The file repeats the
directory's name rather than being called `index` or `mod`, so a stack frame or
a grep names the module instead of the twentieth file to share a name.

This is deliberately *not* the C built-ins. `Math`, `JSON`, `Map`, `Promise`,
`Date` and the string and array methods are in
[`../src/native/`](../src/native/README.md) and need no import. What is here is
the layer above: the things you would otherwise write again in every program.

### Writing a program

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:cli`](cli) | A command line described once — flags, commands, generated help | [README](cli/README.md) |
| [`std:log`](log) | Levels, fields, and a text or JSON line — `Logger` | [README](log/README.md) |
| [`std:errors`](errors) | Kinds of failure and their causes, since `Error` cannot be subclassed | [README](errors/README.md) |

### Testing

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:assert`](assert) | `ok`, `equal`, `deepEqual`, `closeTo`, `throws` — and what a runtime error means for `throws` | [README](assert/README.md) |
| [`std:test`](test) | The runner for those — `suite`, `test`, `run`, `only`, `filter` | [README](test/README.md) |

### Sequences and functions

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:iter`](iter) | Lazily, one element at a time — `range`, `zip`, `chunk`, `groupBy`, `unique` | [README](iter/README.md) |
| [`std:array`](array) | Eagerly, on arrays — `splice`, `sortBy`, `binarySearch`, `dedupe` | [README](array/README.md) |
| [`std:func`](func) | `pipe`, `compose`, `memoize`, `once`, `partial`, `curry2` | [README](func/README.md) |
| [`std:cmp`](cmp) | Comparators, including the string one the language cannot write | [README](cmp/README.md) |
| [`std:range`](range) | An interval as a value — `contains`, `clamp`, `intersect` | [README](range/README.md) |

### Data

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:collections`](collections) | `Deque`, `PriorityQueue`, `Counter`, `DefaultMap` | [README](collections/README.md) |
| [`std:result`](result) | A failure or an absence as a value — `Result`, `Option` | [README](result/README.md) |
| [`std:kind`](kind) | What a value *is* — the question `typeof` cannot answer | [README](kind/README.md) |
| [`std:clone`](clone) | `shallow`, `deep`, `deepReport`, `withFields` | [README](clone/README.md) |
| [`std:cell`](cell) | A value in a box, and one computed once — `Cell`, `Lazy`, `Once` | [README](cell/README.md) |
| [`std:bits`](bits) | The bitwise operations the language does not have, and a `Bitset` | [README](bits/README.md) |

### Text, numbers and time

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:strings`](strings) | `camelCase`, `wrap`, `dedent`, `truncate`, `format` | [README](strings/README.md) |
| [`std:ascii`](ascii) | Characters as codes, and their classification | [README](ascii/README.md) |
| [`std:json`](json) | A parser that fails as a value, and a printer with sorted keys | [README](json/README.md) |
| [`std:stats`](stats) | `mean`, `median`, `stdev`, `quantile`, `describe` | [README](stats/README.md) |
| [`std:random`](random) | Randomness you can seed, and therefore repeat | [README](random/README.md) |
| [`std:time`](time) | Durations, formatting, and a `Stopwatch` | [README](time/README.md) |
| [`std:path`](path) | File paths as pure text — `join`, `dirname`, `relative` | [README](path/README.md) |

### The world outside

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:io`](io) | Files as Results, and the two streams — `readText`, `out`, `StringWriter` | [README](io/README.md) |
| [`std:os`](os) | `env`, `args`, `cwd`, `platform`, `exit` | [README](os/README.md) |

### Asynchrony

| Module | Holds | Reference |
| --- | --- | --- |
| [`std:async`](async) | `sleep`, `timeout`, `retry`, `pool`, `debounce` | [README](async/README.md) |
| [`std:events`](events) | `EventEmitter` | [README](events/README.md) |
| [`std:sync`](sync) | Ordering async work — `Mutex`, `Semaphore`, `WaitGroup`, `Channel` | [README](sync/README.md) |

[`../examples/library.cx`](../examples/library.cx) uses six of them on one
small problem, and is checked against Node on every `make test-node`.

## Where a `std:` module comes from

Resolved against the **executable's own location**, which is the one thing an
installed copy and a source checkout both know about themselves. In order:

| Looked at | For |
| --- | --- |
| `$CSCRIPT_STD_PATH` | A library named explicitly. Set and wrong is an error, not a reason to fall back — using another library after the environment named one would answer with a file it was told not to use |
| `<binary>/../lib/cscript` | An installed tree |
| `<binary>/../../library` | A source checkout, where the binary is `build/<configuration>/cscript` |
| `<binary>/library` | A binary sitting beside the library |

A name is a name, not a path: `std:../../etc/passwd` is refused rather than
resolved. When nothing is found, the error says which of the two things went
wrong — no library at all, or no module of that name in the one that was
found.

## What the language does to how these are written

Every module is ordinary CScript, so everything true of the language is true
here. Four things shaped this code in particular, and each is worth knowing
before adding to it:

**Arity is checked, so a callback is asked how many arguments it wants.**
Passing an index to a one-parameter arrow is an error, not an ignored extra —
so `iter.map` reads `callback.length` and calls with one argument or two,
exactly as the built-in array methods do. Anything here that takes a callback
does the same.

**A runtime error is not catchable.** `assert.throws` sees a `throw` the
program made; it cannot see a property read that failed or a number added to an
object. `Result.attempt` and `async.retry` draw the same line, which is the
language's line: a decision the program took, against a mistake it made.

**A Map cannot be told from a plain object at runtime.** Reading `.size` off
something that is not a Map is a runtime error rather than `undefined`, so
there is no safe test to make. That is why `assert.deepEqual` compares
primitives, arrays and plain objects and nothing else — compare `[...map]`,
which is an array of pairs.

**Lengths are in bytes.** `truncate` defaults to `"..."` rather than an
ellipsis for that reason: `"…"` is three bytes, and would eat three columns of
a width counted in characters.

## Adding a module

1. Write `library/<name>/<name>.cx`, exporting what it offers. It is imported
   as `std:<name>`; no registration anywhere.
2. Write `library/<name>/README.md` beside it: what each export takes and
   answers, then why the code is the way it is. The ten already there are the
   shape to follow.
3. Add a case to [`../tests/cases/stdlib/`](../tests/cases/stdlib) that
   exercises it with `std:assert`, and generate its `.expected` **with Node**:

   ```bash
   tests/std_node.sh tests/cases/stdlib/<name>.cx > tests/cases/stdlib/<name>.expected
   make test          # the same program, run by CScript, must agree
   ```

   [`../tests/std_node.sh`](../tests/std_node.sh) runs a CScript program under
   Node with `std:` rewritten to point at these files, which is what lets Node
   be the oracle for a library that Node has never heard of.
4. Add a row to the table above.

Every case in that group came out of Node byte-for-byte identical to CScript,
seeded random sequences included — **except `io` and `os`**, which Node cannot
run: `fs` is a CScript built-in with no Node counterpart, and `process.env` is
a function here and an object there. Those two expected files were produced by
CScript and checked against their module READMEs, and each README says so.

## Related

- [../docs/JAVASCRIPT.md](../docs/JAVASCRIPT.md) — what differs from JavaScript, and why
- [../docs/GRAMMAR.md](../docs/GRAMMAR.md) — the syntax these are written in
- [../src/native/README.md](../src/native/README.md) — the built-ins underneath, written in C
