# `std:url`

A URL taken apart, and put back together.

```ts
import { parse, format, withParameter } from "std:url";

const url = parse("https://example.com/search?q=a+b").value;
url.query.get("q");                           // "a b"
format(withParameter(url, "page", 2));        // …?q=a%20b&page=2
```

Pure text, like [`std:path`](../path): nothing resolves a name or opens a
connection, because the runtime cannot do either. What it does is the part
every program writing a URL gets slightly wrong — which characters must be
escaped in a query value, what happens to a `+`, and where the `?` goes when
there is nothing after it.

## Specification

### Taking one apart

| Export | Signature | Answers |
| --- | --- | --- |
| `parse` | `(text)` | `Result` of `{ scheme, user, password, host, port, path, query, fragment }` |
| `parseQuery` | `(text)` | `Result` of a **Map**; a repeated name keeps the last |
| `parseQueryAll` | `(text)` | `Result` of a Map of name to **array** |

`port` is -1 when there is none, rather than a guess. `path` is `/` when empty.
`host` is lowered, because that is how hosts are compared.

### Putting one together

| Export | Signature | Answers |
| --- | --- | --- |
| `format` | `(parts)` | the URL; needs a host, defaults the scheme to https |
| `formatQuery` | `(query)` | a Map, an object or an array of pairs, percent-encoded |
| `withParts` | `(parts, changes)` | a copy with some parts replaced |
| `withParameter` | `(parts, name, value)` | a copy with one query parameter set |
| `origin` | `(parts)` | scheme, host and non-default port |

## Implementation

**`query` is a Map, not an object**, because a query string is a mapping and an
object would make a parameter called `toString` a surprise.

**The fragment comes off first.** It may contain anything at all — `?` and `/`
included — and belongs to none of the parts before it.

**A port is only a port when it is digits.** Otherwise an IPv6 host, which is
full of colons, would lose its tail: `https://[::1]:8080/` parses with the host
`[::1]`.

**The default port is left out when formatting.** A URL that writes `:443` is a
different string meaning the same place, which breaks every cache and every
comparison that uses the text as a key.

**A `?` appears only when there is something after it**, and the same for `#`.

**An array value repeats the name** — `{ tag: ["x", "y"] }` becomes
`tag=x&tag=y` — which is the convention every server understands.

**`instanceof Map` is not available** here, since `Map` is a built-in rather
than a class; [`std:kind`](../kind) is how the shape is asked about.

## Source

[`url.cx`](url.cx). `isDigits`, `addPair` and `isDefaultPort` are private.
