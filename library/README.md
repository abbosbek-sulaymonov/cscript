# `library/` — the standard library

Ten modules, written in CScript, imported by name:

```ts
import { range, zip } from "std:iter";
import { mean, stdev } from "std:stats";

console.log(mean([...range(1, 11)]));
```

A `std:` specifier is not a path. It names a module in the library that ships
with the language, so a script anywhere on disk means the same file by it —
everything else must be a relative path, so that reading an import tells you
which file it means.

This is deliberately *not* the C built-ins. `Math`, `JSON`, `Map`, `Promise`,
`Date` and the string and array methods are in
[`../src/native/`](../src/native/README.md) and need no import. What is here is
the layer above: the things you would otherwise write again in every program.

| Module | Holds |
| --- | --- |
| [`std:assert`](assert.cx) | `ok`, `equal`, `deepEqual`, `closeTo`, `throws` — checking what a program believes |
| [`std:iter`](iter.cx) | `range`, `zip`, `chunk`, `groupBy`, `unique` — sequences, lazily, one element at a time |
| [`std:func`](func.cx) | `pipe`, `compose`, `memoize`, `once`, `partial` — building a function out of others |
| [`std:collections`](collections.cx) | `Deque`, `PriorityQueue`, `Counter`, `DefaultMap` — the four structures a Map and an array do not cover |
| [`std:result`](result.cx) | `Result`, `Option` — a failure or an absence as a value rather than a throw |
| [`std:strings`](strings.cx) | `camelCase`, `wrap`, `dedent`, `truncate`, `format` — the string work that is not one method call |
| [`std:stats`](stats.cx) | `mean`, `median`, `stdev`, `quantile`, `describe` — summarising numbers |
| [`std:random`](random.cx) | `Random` — randomness you can seed, and therefore repeat |
| [`std:events`](events.cx) | `EventEmitter` — one thing announcing, several listening |
| [`std:async`](async.cx) | `sleep`, `timeout`, `retry`, `pool`, `debounce` — waiting, and not doing everything at once |

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

1. Write `library/<name>.cx`, exporting what it offers. It is imported as
   `std:<name>`; no registration anywhere.
2. Add a case to [`../tests/cases/stdlib/`](../tests/cases/stdlib) that
   exercises it with `std:assert`, and generate its `.expected` **with Node**:

   ```bash
   tests/std_node.sh tests/cases/stdlib/<name>.cx > tests/cases/stdlib/<name>.expected
   make test          # the same program, run by CScript, must agree
   ```

   [`../tests/std_node.sh`](../tests/std_node.sh) runs a CScript program under
   Node with `std:` rewritten to point at these files, which is what lets Node
   be the oracle for a library that Node has never heard of.
3. Add a row to the table above.

Every case in that group came out of Node byte-for-byte identical to CScript,
seeded random sequences included.

## Related

- [../docs/JAVASCRIPT.md](../docs/JAVASCRIPT.md) — what differs from JavaScript, and why
- [../docs/GRAMMAR.md](../docs/GRAMMAR.md) — the syntax these are written in
- [../src/native/README.md](../src/native/README.md) — the built-ins underneath, written in C
