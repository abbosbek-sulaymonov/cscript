# `std:cache`

Remembering a value, and forgetting it on purpose.

```ts
import { LruCache } from "std:cache";

const pages = new LruCache(100, { ttl: 60000 });
const page = pages.fetch(url, () => render(url));
```

A Map remembers everything, which is the same as leaking when the keys keep
coming. A cache is a Map with a rule for what to throw away: the one used
longest ago, or anything older than a given age.

[`func.memoize`](../func) is the version with no rule at all — right for a pure
function over a small domain, wrong for anything keyed by a request or a
filename.

## Specification

`new LruCache(capacity, { ttl, clock })` — `ttl` in milliseconds, zero meaning
no expiry.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `get` | `(key)` | the value, **marking it used**, or `undefined` |
| `set` | `(key, value)` | stores, evicting the least recently used if full |
| `has` | `(key)` | whether it is there — deliberately **not** a use |
| `fetch` | `(key, build)` | the cached value, or builds and caches one |
| `delete` / `clear` | `(key)` / `()` | remove one, or all |
| `prune` | `()` | drops everything expired; answers how many |
| `keys` | `()` | least recently used first |
| `size` / `capacity` | getters | how many it holds, and how many it may |
| `stats` | getter | `{ hits, misses, evictions, hitRate }` |

| Export | Signature | Answers |
| --- | --- | --- |
| `memoize` | `(fn, { capacity, ttl, clock, keyOf })` | a function that remembers, with a bound |

## Implementation

**A Map keeps its insertion order, and `delete` then `set` moves a key to the
end** — so "the one used longest ago" is simply the first key the Map hands
back, and no separate linked list is needed. That is why this is thirty lines
rather than a hundred and thirty.

**Reading marks an entry as used; asking whether it exists does not.** That is
what makes it least-recently-*used* rather than least-recently-added, and
`has` stays honest: asking whether something is cached is not wanting it.

**`fetch` is one call** because the two halves — look, then build and store —
drift apart when they are written separately, and the version that drifts
stores under a different key than it looked up.

**`stats` exists because a cache is worth measuring.** A hit rate near zero is a
cache that costs memory and buys nothing, and nothing else will tell you.

**The clock is injectable**, for the same reason as in [`std:log`](../log): a
cache whose expiry cannot be reproduced cannot be tested.

## Source

[`cache.cx`](cache.cx). Nothing private beyond the entry shape.
