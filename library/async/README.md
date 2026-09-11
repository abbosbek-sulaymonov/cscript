# `std:async`

Waiting, retrying, and not doing everything at once.

```ts
import { sleep, timeout, retry, pool } from "std:async";

await sleep(50);
const page = await timeout(fetchIt(), 2000, "fetch took too long");
const rows = await pool(ids, 4, async (id: number) => load(id));
```

## Specification

### Waiting

| Export | Signature | Answers |
| --- | --- | --- |
| `sleep` | `(milliseconds)` | a promise that resolves with nothing |
| `delay` | `(value, milliseconds)` | the same wait, resolving with the value |
| `timeout` | `(work, milliseconds, message = "timed out")` | the work's value, or **rejects** when it is late |

### Retrying

| Export | Signature | Answers |
| --- | --- | --- |
| `retry` | `(body, options?)` | what the body finally answered, or re-throws the last failure |

`options` is `{ attempts = 3, delayMs = 0, factor = 2, onRetry }`, all optional.
The delay grows by `factor` each time; `onRetry` is called with the error, and
with the attempt number when it declares two parameters.

### Doing several things

| Export | Signature | Answers |
| --- | --- | --- |
| `pool` | `(items, limit, worker)` | every result **in input order**, at most `limit` in flight |
| `settleAll` | `(work)` | `{ ok, value }` or `{ ok, error }` per item, in order |

`worker` receives the index as a second argument when it declares one.

### Rate

| Export | Signature | Answers |
| --- | --- | --- |
| `debounce` | `(fn, milliseconds)` | `{ run, cancel }` — waits for quiet before calling |
| `throttle` | `(fn, milliseconds)` | a function that calls at most once per interval |
| `deferred` | `()` | `{ promise, resolve, reject }` |

## Implementation

**A timer keeps the program alive until it fires**, so everything here that
starts one also cancels it. `timeout` that left its own timer running would
make a program that finished its work sit there for the length of the timeout
before exiting — which is why it clears the timer on both paths.

**`timeout` does not cancel the work.** A promise cannot be cancelled, so this
bounds how long the *caller* waits, not how long the work runs.

**`retry` re-throws the last failure rather than wrapping it**, so the caller
sees what actually went wrong instead of "retried 3 times". The backoff matters:
retrying a struggling service immediately is how a stumble becomes an outage.

**`pool` answers results in input order**, whatever order they finished in — an
unordered result would make the caller match them up again, which is the work
this was meant to save. The limit exists because `Promise.all` over ten
thousand items starts ten thousand of them at once.

**`debounce` answers an object, not a function with a `cancel` on it**, because
CScript functions do not hold properties. Cancelling matters: a debounced call
still pending when the work is done keeps the program alive until its timer
fires.

**`deferred` exists for the times the thing that settles a promise is not the
thing that created it** — a callback API, an event handler. Without it the pair
have to be smuggled out of the executor.

**What none of this catches.** A runtime error is not catchable in CScript, so
nothing here will retry one. That is the intended line: a failure worth
retrying is one the code decided to report.

## Source

[`async.cx`](async.cx). Nothing private; `sleep` is the one everything else
waits through.
