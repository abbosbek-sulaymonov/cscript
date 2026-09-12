# `std:errors`

Kinds of failure, and what caused what.

```ts
import { kind, wrap, isKind, format } from "std:errors";

const NotFound = kind("NotFound");
throw NotFound("no user " + String(id), { id: id });

try { load(); } catch (e) { throw wrap(e, "cannot start"); }
```

**CScript cannot subclass `Error`.** `class NotFound extends Error` is refused —
`Error` is a built-in function rather than a class — so the usual way of
building a taxonomy of failures is unavailable, and programs end up comparing
`error.message` against a string. That is the worst possible test: it breaks
when the wording improves.

So a kind is a **name**, carried on the error and asked about by name.

## Specification

### Making one

| Export | Signature | Answers |
| --- | --- | --- |
| `make` | `(kind, message, details?)` | an Error with `name` set, and `details` when given |
| `kind` | `(name)` | a maker for that kind, so a module names its failures once |
| `wrap` | `(error, message, asKind?)` | a new error with `cause` set; takes the wrapped kind unless told otherwise |

### Asking

| Export | Signature | Answers |
| --- | --- | --- |
| `kindOf` | `(error)` | the name, `"Error"` for anything without one |
| `messageOf` | `(error)` | the message, even for a thrown string |
| `detailsOf` | `(error)` | the details object, or `undefined` |
| `isKind` | `(error, name)` | whether this **or anything under it** is of that kind |
| `find` | `(error, name)` | the **innermost** error of that kind |
| `chain` | `(error)` | every error from this one down |
| `rootCause` | `(error)` | the one underneath everything |

### Printing

| Export | Signature | Answers |
| --- | --- | --- |
| `describe` | `(error)` | `NotFound: no user 7`, one line |
| `format` | `(error, indent = "  ")` | the whole chain, innermost last |

### Handling

| Export | Signature | Behaviour |
| --- | --- | --- |
| `catching` | `(body, branches, otherwise?)` | runs the body, sends a throw to the branch for its kind |

```ts
catching(() => load(path), {
  NotFound: () => defaults,
  Permission: (e: any) => { throw wrap(e, "cannot read the config"); },
});
```

## Implementation

**A wrapper inherits the kind of what it wraps**, so a handler asking
`isKind(e, "NotFound")` still finds it after three layers have added context —
and `describe` shows a useful kind at every level rather than a bare `Error`.
Pass `asKind` where a layer genuinely changes what the failure *is*.

**`find` answers the innermost match, not the outermost.** Because wrapping
inherits the kind, the outermost match is usually the layer that added context
and the innermost is the failure itself — which is where the details are. A
handler wants the error that knows the id, not the one that knows it could not
start.

**A cycle is stopped rather than followed.** `a.cause = b; b.cause = a` is a
mistake; a walk that hung on it would be a worse one.

**Anything can be thrown, so anything can be asked about.** `kindOf("text")`
and `kindOf(null)` answer `"Error"` rather than failing, because a thrown value
is whatever the program threw.

**`catching` sees only what was thrown.** A runtime error — reading a property
that is not there, adding a number to an object — is not catchable in CScript,
and no branch will ever see one.

## Source

[`errors.cx`](errors.cx). Nothing private.
