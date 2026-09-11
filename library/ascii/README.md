# `std:ascii`

Characters as codes, and the questions one asks about them.

```ts
import { isDigit, digitValue, char, code, toUpper } from "std:ascii";

if (isDigit(text[i])) total = total * 10 + digitValue(text[i]);
```

CScript's strings are bytes: `text[i]` is one byte and `charCodeAt` answers its
value, so for ASCII a character and a byte are the same thing. This module is
the 128 codes, their classification, and the conversions between a character
and its number.

## Specification

### Between a character and its code

| Export | Signature | Answers |
| --- | --- | --- |
| `code` | `(text)` | the code of the first character, or **-1** for an empty string |
| `char` | `(value)` | the one-character string; throws outside 0…255 |
| `codes` | `(text)` | every code in the string |
| `fromCodes` | `(values)` | the string those codes spell |

### Classification

Each takes a one-character string **or** a code.

| Export | True for |
| --- | --- |
| `isAscii` | 0…127 |
| `isDigit` | `0`–`9` |
| `isUpper` / `isLower` | `A`–`Z` / `a`–`z` |
| `isAlpha` | either case |
| `isAlphanumeric` | a letter or a digit |
| `isSpace` | space, tab, newline, carriage return, vertical tab, form feed |
| `isPunctuation` | printable, and neither a letter, a digit, nor a space |
| `isControl` | the 32 below space, and DEL |
| `isPrintable` | 32…126 |
| `isHexDigit` | `0`–`9`, `a`–`f`, `A`–`F` |

### Case and numbers

| Export | Signature | Answers |
| --- | --- | --- |
| `toUpper` / `toLower` | `(value)` | the character, cased |
| `digitValue` | `(value)` | 0…9, or **-1** |
| `hexValue` | `(value)` | 0…15, or **-1** |

### Whole strings

| Export | Signature | Answers |
| --- | --- | --- |
| `isAsciiText` | `(text)` | whether every byte is below 128 |
| `stripControl` | `(text, replacement = "")` | control characters replaced |

## Implementation

**Nothing here is Unicode-aware, deliberately.** A byte above 127 is part of a
multi-byte sequence whose meaning cannot be decided one byte at a time, so
every predicate answers false for it rather than guessing.

**The classification is arithmetic, not a table or a regular expression.** The
ranges *are* the definition; a table would be a second copy of them to keep in
step.

**`code("")` is -1 rather than 0**, because 0 is NUL — a real character — and
answering it for "nothing" would make the two indistinguishable. `digitValue`
and `hexValue` answer -1 for the same reason, rather than NaN: -1 composes
without a separate check.

**`toUpper` is arithmetic on one character.** The 32 between `A` and `a` is the
whole of ASCII case. `toUpperCase` on a string does this and more, so these are
for code already working in codes.

**`isAsciiText` is worth calling before any byte-level algorithm**, because
that is exactly the assumption such an algorithm makes.

**`stripControl` exists for text from somewhere else**, which can otherwise
rearrange a terminal with a stray escape sequence.

## Source

[`ascii.cx`](ascii.cx). `codeOf` is private: the one place a character-or-code
argument becomes a number.
