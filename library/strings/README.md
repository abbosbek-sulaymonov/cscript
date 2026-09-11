# `std:strings`

The string operations that are not one method call.

```ts
import { camelCase, wrap, dedent, format } from "std:strings";

console.log(camelCase("parse_http_response"));        // parseHttpResponse
console.log(wrap("the quick brown fox", 10));         // ["the quick", "brown fox"]
console.log(format("{name} is {age}", { name: "Ada", age: 36 }));
```

A string already answers `split`, `trim`, `padStart` and the rest. What is here
is the things built out of those that everyone writes again.

## Specification

### Taking one apart

| Export | Signature | Answers |
| --- | --- | --- |
| `words` | `(text)` | the words in a name, whatever convention wrote it |
| `lines` | `(text)` | split on newlines |

### Naming conventions

| Export | `"parse_http_response"` becomes |
| --- | --- |
| `capitalize` / `uncapitalize` | first character up or down, rest untouched |
| `titleCase` | `Parse Http Response` |
| `camelCase` | `parseHttpResponse` |
| `pascalCase` | `ParseHttpResponse` |
| `kebabCase` | `parse-http-response` |
| `snakeCase` | `parse_http_response` |

### Fitting one to a shape

| Export | Signature | Answers |
| --- | --- | --- |
| `truncate` | `(text, width, ending = "...")` | at most `width` bytes, **including** the ending |
| `padCenter` | `(text, width, filler = " ")` | padded both sides, extra to the right |
| `wrap` | `(text, width)` | an array of lines, broken on spaces |
| `dedent` | `(text)` | the shared leading indentation removed |
| `indent` | `(text, by = 2, filler = " ")` | every non-empty line prefixed |

### Asking about one

| Export | Signature | Answers |
| --- | --- | --- |
| `isBlank` | `(text)` | whether it is empty or only whitespace |
| `count` | `(text, needle)` | how many **non-overlapping** times it appears |
| `reverse` | `(text)` | byte order reversed |
| `ensurePrefix` / `ensureSuffix` | `(text, affix)` | added if absent |
| `stripPrefix` / `stripSuffix` | `(text, affix)` | removed if present |
| `format` | `(pattern, values)` | `{name}` replaced from the object |

## Implementation

**Everything works on bytes, not code points**, which is what CScript's own
string methods do. For ASCII — identifiers, keys, log lines — the two are the
same thing. `std:ascii` is the module that cares about the distinction.

**`words` treats a run of capitals as one word up to the last of them**, because
that last capital starts the next: `HTTPResponse` is `HTTP` and `Response`, not
`H`, `T`, `T`, `P`, `Response`. Separators are space, `_`, `-`, `.`, `/`, tab
and newline.

**`truncate` defaults to three dots rather than an ellipsis** because lengths
are in bytes: `"…"` is three of them, so an ellipsis would eat three columns of
a width counted in characters. It cuts to `width` *including* the ending — a
truncation that overflows the column it was cut for is not a truncation.

**`wrap` never breaks inside a word**, except a word longer than the width,
which has nowhere else to break and is left to overflow rather than split into
something that is no longer the word.

**`dedent` ignores blank lines** when working out what is shared, because a
blank line indents to nothing.

**`format` leaves an unknown name alone** rather than writing `undefined`, so a
mistake in the pattern survives to be seen. It exists for a pattern that
arrives as *data* — from a config file, a translation table — which is exactly
the case a template literal cannot serve, because it is compiled.

## Source

[`strings.cx`](strings.cx). Nothing private.
