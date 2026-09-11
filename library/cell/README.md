# `std:cell`

A value in a box, and a value computed once.

```ts
import { Cell, Lazy, Once } from "std:cell";

const count = new Cell(0);
count.update((n: number) => n + 1);

const config = new Lazy(() => expensiveRead());
const setup = new Once();
```

`const` prevents rebinding, not mutation, and `let` allows both — so there is
no way in the language to say "this binding never changes, and what it holds
does". A Cell is that: the binding is `const` and the contents are asked for
and set through methods, which makes every change a call that can be found by
searching.

## Specification

### `Cell`

`new Cell(value?)`

| Member | Signature | Behaviour |
| --- | --- | --- |
| `get` | `()` | what it holds |
| `set` | `(value)` | replaces it; answers the Cell, so calls chain |
| `update` | `(transform)` | a read and a write as one call; answers the new value |
| `replace` | `(value)` | sets it and answers the **old** value |
| `take` | `()` | answers the old value and leaves `undefined` |
| `isEmpty` | getter | whether it holds `undefined` |

### `Lazy`

`new Lazy(build)` — `build` runs the first time `get` is called, and not before.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `get` | `()` | the value, building it once |
| `isBuilt` | getter | whether it has been |
| `reset` | `()` | forgets it, so the next `get` builds again |

### `Once`

`new Once()`

| Member | Signature | Behaviour |
| --- | --- | --- |
| `run` | `(body)` | runs the body at most once; answers what it answered, every time |
| `hasRun` | getter | whether it has |

## Implementation

**A Cell is the one way to share a mutable value across closures** without
relying on an enclosing `let`. Two functions that both hold the same Cell see
each other's writes, and neither has to be defined in the same scope.

**`update` exists so a read and a write cannot drift apart.** Written as two
statements, the expression between them grows and eventually something else
reads the old value in the middle.

**`Lazy` keeps a flag rather than testing the value**, so a builder that
answered `undefined` is not run again on every `get`. The difference from
`func.memoize` is that there are no arguments: a Lazy holds one value, not a
table of them. The difference from a plain `const` is *when* — an expensive
value needed on only some paths costs nothing on the others.

**`Once` is `func.once` as an object**, for when the guard has to be handed
around or asked whether it has fired.

## Source

[`cell.cx`](cell.cx). Nothing private.
