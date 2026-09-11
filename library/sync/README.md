# `std:sync`

Ordering asynchronous work.

```ts
import { Mutex, Semaphore, WaitGroup, Channel } from "std:sync";

const lock = new Mutex();
await lock.withLock(async () => { await readModifyWrite(); });
```

The names come from languages with threads, and here they mean something
narrower that is worth being precise about.

## What these do and do not do

**CScript is single-threaded.** Nothing in this module protects memory, because
nothing can interleave in the middle of a statement: between two `await`s a
function has the machine to itself.

What these order is **resumptions**. Two async functions that both `await` the
same thing will both continue, in some order, and if each reads then writes a
shared value across that `await` they can still lose an update — not because
two things ran at once, but because one ran between the read and the write of
the other:

```ts
let shared = 0;
async function increment() {
  const seen = shared;   // reads 0
  await sleep(1);        // the other two run here, and read 0 as well
  shared = seen + 1;     // all three write 1
}
await Promise.all([increment(), increment(), increment()]);   // shared is 1
```

A Mutex makes that section run to completion before another enters it, and
`shared` ends at 3. That is the whole guarantee.

## Specification

### `Mutex` — one holder at a time

| Member | Signature | Behaviour |
| --- | --- | --- |
| `lock` | `()` | a promise for the **release function** |
| `withLock` | `(body)` | takes the lock, runs the body, releases it — even if it throws |
| `isHeld` | getter | whether anyone holds it |
| `waiterCount` | getter | how many are waiting |

### `Semaphore` — at most *n* holders

`new Semaphore(permits)`

| Member | Signature | Behaviour |
| --- | --- | --- |
| `acquire` | `()` | a promise for the release function |
| `withPermit` | `(body)` | the same guarantee as `withLock` |
| `available` / `waiterCount` | getters | permits free, and waiters |

### `WaitGroup` — wait for a set to finish

| Member | Signature | Behaviour |
| --- | --- | --- |
| `add` | `(howMany = 1)` | counts work in |
| `done` | `()` | counts one out; throws if nothing is pending |
| `wait` | `()` | a promise that resolves when the count reaches zero |
| `run` | `(body)` | `add`, run, `done` — including on a throw |
| `pending` | getter | how many are outstanding |

### `Channel` — values from a producer to a consumer

`new Channel(capacity = 0)` — zero means unbounded.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `send` | `(value)` | a promise that resolves once taken or queued; **waits** when full |
| `receive` | `()` | a promise for `{ value, done }`; **waits** when empty |
| `close` | `()` | nothing more will be sent; queued values are still delivered |
| `drain` | `()` | everything until closed and drained, as an array |
| `length` / `isClosed` | getters | how many are queued, and whether it is closed |

## Implementation

**`lock` answers a release function rather than taking one back through a
method**, so the thing that must be called is in the hands of whoever holds the
lock. `withLock` exists because a release that must be called is a release that
will be forgotten on some path — and it uses `finally`, so a throw inside the
body does not leave the lock held for ever.

**The lock is handed straight to the next waiter**, not released for them to
race for. A gap between the two would let anything else that happened to be
scheduled in between jump the queue. First in, first served, which is what
makes a queue of writers finish.

**A `WaitGroup` counts rather than holding promises.** Where `Promise.all`
needs every promise in hand at once, this can be waited on by work that starts
more work. More `done`s than `add`s throws, because the alternative is a group
that answers "finished" while work is still running.

**A `Channel` hands a value straight to a waiting receiver**, which keeps a
rendezvous from costing a trip through the queue. A bounded one makes the
producer wait, which is the point of having a capacity: back-pressure. Closing
tells every waiting receiver it is finished and **rejects** any sender still
holding a value, rather than leaving it waiting on a promise nothing will
settle.

## Source

[`sync.cx`](sync.cx). Built on `deferred` from [`std:async`](../async); the
release functions are private methods.
