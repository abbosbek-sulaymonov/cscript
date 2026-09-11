# `std:events`

One thing announcing, several things listening.

```ts
import { EventEmitter } from "std:events";

const bus = new EventEmitter();
const off = bus.on("line", (text: string) => console.log("heard:", text));
bus.emit("line", "hello");
off();
```

The point of an emitter is that the part which knows something happened does
not have to know who cares.

## Specification

### `EventEmitter`

| Member | Signature | Behaviour |
| --- | --- | --- |
| `on` | `(event, listener)` | registers, and **answers a function that removes it** |
| `once` | `(event, listener)` | the same, for one delivery only |
| `off` | `(event, listener)` | removes the first registration of that listener; answers whether it found one |
| `emit` | `(event, payload?)` | calls every listener; answers whether there were any |
| `listenerCount` | `(event)` | how many are registered |
| `eventNames` | `()` | the events that have listeners |
| `removeAllListeners` | `(event?)` | for one event, or all of them |
| `next` | `(event)` | a **Promise** for the next payload, so an emitter can be awaited |

## Implementation

**An event carries one payload, not a variadic list.** CScript checks arity, so
an emitter passing three arguments would be an error at every ordinary
one-parameter listener rather than an ignored extra. A listener that declares
no parameters is called with none — the private `callListener` reads
`listener.length` to decide.

**A listener that throws stops the emit, and the throw reaches the caller of
`emit`.** Swallowing it would be the more forgiving choice and the wrong one:
an exception nobody sees is how a broken listener stays broken.

**`on` answers a remover** so an anonymous arrow can still be taken off,
without the caller having to hold the same reference to pass to `off`.

**`once` removes itself before running**, so a listener that emits the same
event again does not re-enter it. It uses a `let` bound wrapper rather than a
`const` one, because an arrow bound with `const` cannot see its own binding
from inside itself — and this one has to, in order to take itself off.

**`emit` walks a copy of the list.** A listener that adds or removes one does
not change what this emit delivers; the alternative is a listener being skipped
or run twice depending on where it sat in the array.

**`off` rebuilds the array rather than splicing it**, because CScript's arrays
have no `splice` — and a rebuild is the same O(n) a splice would have been.

## Source

[`events.cx`](events.cx). `callListener` is private: the arity adaptation every
delivery goes through.
