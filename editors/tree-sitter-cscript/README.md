# tree-sitter-cscript

A [tree-sitter](https://tree-sitter.github.io) grammar for CScript, for editors
that parse rather than pattern-match: Neovim, Helix, Zed, and anything else
built on the same parser.

Transcribed from `docs/GRAMMAR.md` in the compiler repository, which is what
the parser implements. Where CScript differs from JavaScript this differs too,
deliberately — there is no `var`, no `==`, no automatic semicolon insertion,
and a declaration may carry a type annotation. Leaving those in *because
JavaScript has them* would make the editor accept what the compiler refuses,
which is the failure this grammar exists to avoid.

## State: a working scaffold, not finished

Honest numbers, reproducible with the command below:

| | |
| --- | --- |
| Valid programs parsed | **99 of 113** |
| Programs the compiler rejects, and this does too | 5 of 44 |
| Corpus tests | 4, all passing |
| `queries/highlights.scm` | loads, 81 lines |

`examples/tour.cx` exercises every construct the grammar claims — classes,
private fields, static blocks, accessors, generators, `for await`,
destructuring with defaults and rest, labels, `switch`, `try`/`catch`/
`finally`, optional chaining, `??`, BigInt, named regex groups, imports and
exports — and parses cleanly. It also runs under `cscript`, which is the
check that keeps it honest.

### What does not parse yet

- **Arrow functions with default parameters.** `(a = 7) => a` fails, while
  `function f(a = 7) {}` and `(a, b) => a` both work. The parser commits to a
  parenthesised expression at the `(` and cannot back out at the `=>`.
  tree-sitter reports the conflict declaration for it as unnecessary, which
  means the ambiguity is not where it looks — this needs the shared-rule
  treatment tree-sitter-javascript uses, not another conflict.
- **`async *[computed]()` methods.** `async *name()` works; the computed form
  does not.
- Twelve further files, most of which fail downstream of one of the above.

The 39 error cases that *do* parse are correct: most assert a semantic refusal —
`1 == "1"`, `let x: integer` — which is the checker's job, not the grammar's.

## Checking it

```bash
npm install
npx tree-sitter generate
npx tree-sitter test

# Against every program in the repository, error cases separated because a
# failure there is the right answer.
for f in $(find ../../tests/cases ../../examples -name '*.cx' | grep -v /errors/); do
  npx tree-sitter parse -q "$f" >/dev/null 2>&1 || echo "FAILS $f"
done
```

## Layout

```
grammar.js          the grammar, from docs/GRAMMAR.md
queries/
  highlights.scm    what to colour
  locals.scm        scopes and definitions, for rename and go-to-definition
  injections.scm    regex patterns, and jsdoc comments
test/corpus/        parse trees asserted exactly
examples/tour.cx    every construct, and it runs
```

## Relationship to the TextMate grammar

`editors/vscode-cscript/syntaxes/cscript.tmLanguage.json` is a separate
implementation for VS Code, which does not use tree-sitter. The two make the
same distinctions — a BigInt is not a number, a type annotation is not an
identifier — but they cannot share a definition, because one is a parser and
the other is a set of regular expressions. Both are derived from the same
`docs/GRAMMAR.md`, which is the only way to keep them saying the same thing.
