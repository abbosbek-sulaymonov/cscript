# Changelog

## 0.1.0

First release.

- CScript language for `.cx`, with the id `cscript` and the scope
  `source.cscript`.
- A TextMate grammar derived from `docs/GRAMMAR.md`, which is what the parser
  implements. It distinguishes BigInt literals from ordinary numbers, regular
  expressions from division, `#private` names, labels, `static` blocks,
  `new.target`, and CScript's type annotations — the last being the one part of
  the syntax that is not JavaScript.
- Comment, bracket, auto-closing, surrounding, folding and indentation rules.
- Sixteen snippets, each of a form the compiler accepts.
- A file icon theme, for the one way an extension can set a file icon.
