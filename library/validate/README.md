# `std:validate`

Checking that data from outside is the shape it claims.

```ts
import { object, string, number, optional, array } from "std:validate";

const user = object({
  name: string().min(1),
  age: number().integer().min(0),
  tags: optional(array(string()), []),
});

user.check(parsed).unwrapOr(defaults);
```

CScript's type checker proves what it can see. It cannot see a parsed JSON
body, a row from a CSV file or an environment variable — those arrive as `any`,
and the annotation on the function receiving one is a comment. This is the
check that makes it true at the boundary, once, so everything inside can rely
on it.

## Specification

### Builders

| Export | Signature | Accepts |
| --- | --- | --- |
| `string` / `number` / `boolean` | `()` | that kind |
| `array` | `(inner)` | an array whose elements pass `inner` |
| `object` | `(shape)` | an object whose fields pass theirs |
| `optional` | `(inner, fallback?)` | absent, or `inner` |
| `anything` | `()` | any value — still **required** unless made optional |
| `literal` | `(allowed)` | one of a fixed set |

### Constraints

Chained onto any builder, each answering a **new** schema:

`min`, `max` (a number's value, or a string's or array's length), `length`,
`integer`, `matches(pattern, described?)`, `oneOf(allowed)`,
`check_(test, message)` for anything else.

### Running

| Member | Signature | Answers |
| --- | --- | --- |
| `check` | `(value)` | `Result` of the value, or of `[{ path, message }, …]` |
| `explain` | `(value)` | the same, with errors as `"path: message"` lines |

```
name: must have at least 1 of them
age: must be at least 0
role: must be one of admin, user
```

## Implementation

**Every error, not the first.** One at a time is what makes filling in a form
take six attempts, and a validator that stops early cannot tell a caller
whether the rest was fine.

**The path says where, however deep**: `xs[1].n` for the second element's
field. That is the difference between a message a user can act on and one that
says a body is invalid.

**An unknown key is dropped, not refused.** A body with an extra field is the
normal way an API grows, and refusing it makes an old client break against a
new server. What comes back is the *checked* shape, so the extra never reaches
the code inside.

**A builder answers a new schema rather than changing this one**, so a schema
shared between two shapes cannot be altered by either — the trap in every
builder API that mutates.

**`null` counts as absent**, because a JSON body writes a missing field that way
as often as it leaves it out.

**It does not coerce.** `"5"` is not a number: a validator that quietly turns
one into the other is the reason a string ends up in a database column that
was supposed to hold integers.

## Source

[`validate.cx`](validate.cx). `Schema` is the one class; `run` is the private
walk that produces the errors.
