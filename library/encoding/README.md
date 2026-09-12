# `std:encoding`

The four ways bytes get written down as text.

```ts
import { encodeBase64, toHex, percentEncode, characters } from "std:encoding";

encodeBase64("hello world");   // "aGVsbG8gd29ybGQ="
percentEncode("a b&c");        // "a%20b%26c"
characters("café").value;      // ["c", "a", "f", "é"]
```

Base64 for anything binary travelling as text, hex for anything a person will
read, percent-encoding for URLs, and UTF-8 for the gap between bytes and
characters. None of `btoa`, `atob`, `encodeURIComponent` or `TextDecoder`
exists in the runtime.

## Specification

### Base64

| Export | Signature | Answers |
| --- | --- | --- |
| `toBase64` / `encodeBase64` | `(bytes)` / `(text)` | standard base64, padded |
| `toBase64Url` | `(bytes)` | `-` and `_`, unpadded — survives a URL or a filename |
| `fromBase64` / `decodeBase64` | `(text)` | `Result` of the bytes / of the text |

The decoder accepts **both** alphabets and treats padding as optional: one that
refused the URL-safe form would fail on half the tokens in the world.

### Hex

| Export | Signature | Answers |
| --- | --- | --- |
| `toHex` | `(bytes, upper = false)` | the digits |
| `fromHex` | `(text)` | `Result` of the bytes; a `0x` prefix is allowed |

### Percent-encoding

| Export | Signature | Answers |
| --- | --- | --- |
| `percentEncode` | `(text)` | everything outside the RFC 3986 unreserved set escaped |
| `percentDecode` | `(text)` | `Result` of the text; **`+` is a space** |

### UTF-8 — the one that matters most here

| Export | Signature | Answers |
| --- | --- | --- |
| `decodeUtf8` | `(text)` | `Result` of the code points, as numbers |
| `encodeUtf8` | `(code)` | the bytes of one code point |
| `characters` | `(text)` | `Result` of the characters, as strings |
| `characterLength` | `(text)` | `Result` of how many there are |
| `isValidUtf8` | `(text)` | whether the bytes are valid UTF-8 at all |

## Implementation

**A CScript string is bytes**, so `"café".length` is 5 and `text[3]` is half of
an é. Everything in the library that walks a string walks bytes — right for
ASCII and wrong the moment text arrives from a person. These four functions are
the way across.

**Base64 is arithmetic, not shifting.** The language has no shift operators, and
24 bits fits in a double exactly. The tail cases are where such an
implementation goes wrong — two characters carry one byte, three carry two —
so the test round-trips **every** length from 0 to 39 rather than the one
someone tried by hand.

**`percentEncode` uses the unreserved set only.** `encodeURI`'s laxer set is not
offered, because choosing between the two wrongly is the usual bug.

**`percentDecode` treats `+` as a space**, because that is what a form body
means by it, and a decoder that ignores it silently mangles every query string
it is given.

**Everything that reads input answers a `Result`.** Base64, hex, percent and
UTF-8 all arrive from outside, and a runtime error is not catchable.

## Node cannot check the non-ASCII half

`tests/cases/stdlib/encoding.cx` has its expected output produced by CScript, for
the same reason as [`std:bytes`](../bytes): a Node string is UTF-16, so
`percentEncode("é")` is `%E9` there and `%C3%A9` here. The second is correct for
a UTF-8 world; the difference is the string model, not the encoder.

## Source

[`encoding.cx`](encoding.cx). `encodeWith`, `base64Value`, `hexValue` and
`isUnreserved` are private.
