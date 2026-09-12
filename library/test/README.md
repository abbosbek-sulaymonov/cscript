# `std:test`

Running the checks that [`std:assert`](../assert) makes.

```ts
import { suite, test, run, exitCode } from "std:test";
import { equal } from "std:assert";
import { exit } from "std:os";

suite("Deque", () => {
  test("pushes", () => { equal(new Deque([1]).size, 1); });
  test("pops nothing when empty", () => { equal(new Deque().popFront(), undefined); });
});

exit(exitCode(await run()));
```

`std:assert` is the assertions; this is the part that collects them, runs them,
catches what they throw, and reports. Without it every test file is a script
that stops at the first failure and says nothing about the twelve cases after
it.

## Specification

### Registering

| Export | Signature | Behaviour |
| --- | --- | --- |
| `test` | `(name, body)` | registers; the body may be async and is awaited |
| `skip` | `(name, body)` | registered, reported, not run |
| `only` | `(name, body)` | when any test is `only`, every other one is skipped |
| `suite` | `(name, body)` | groups names; runs its body immediately to register what is inside |

### Running

| Export | Signature | Answers |
| --- | --- | --- |
| `run` | `(options?)` | `{ passed, failed, skipped, total, failures, ok }` |
| `exitCode` | `(summary)` | 0 when everything passed, 1 otherwise |
| `line` | `(summary)` | `"3 passed, 1 failed"` |
| `names` | `()` | what is registered, without running any of it |
| `filter` | `(text)` | keeps only the tests whose name contains it |
| `reset` | `()` | forgets every registration |

`run` takes `{ report, quiet }` — `report` is where each line goes,
`console.log` by default and any one-argument function otherwise.

### What it prints

```
ok    Deque › pushes
FAIL  Deque › pops nothing when empty
        undefined !== 0

1 passed, 1 failed
```

## Implementation

**The registry is module-level**, which lets a test file be a file of `test(...)`
calls with no ceremony: importing it is the registration.

**`run` answers rather than exits.** A runner that called `exit` could not be
tested by itself, and the program may have more to do — so the exit code is
`exitCode(summary)`, applied by the caller.

**A body is awaited whether or not it is async.** Awaiting a plain value
answers it, so one path covers both, and an async body that rejects fails that
one test rather than the run.

**A skipped test is a decision that shows up in the summary**; a commented-out
one is a decision nobody can see. `only` exists for the same reason — narrowing
a run without deleting the rest.

**`filter` is applied before the run**, so the summary counts what was asked for
rather than what was registered.

**No setup or teardown hooks, on purpose.** A test that needs fixtures builds
them in its own body, where what it did is visible. Shared mutable setup is the
thing that makes a failing test hard to read.

**Something that is not an assertion fails differently.** An `AssertionError`
prints its message alone — the kind adds nothing, since that is what a failing
test is — while anything else is named, because a test that threw unexpectedly
failed in a different way from one whose check did not hold.

## What a failure cannot be

A **runtime error** — reading a property that is not there, adding a number to
an object — is not catchable in CScript, so it ends the whole run rather than
failing one test. Worth knowing when a run stops with output missing: the last
test named is the one that did it.

## Source

[`test.cx`](test.cx). `fullName` and `describeFailure` are private.
