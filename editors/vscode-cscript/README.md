# CScript Language Support

Syntax highlighting, editing behaviour and snippets for
[CScript](https://github.com/abbosbek-sulaymonov/cscript) — a gradually typed
language with JavaScript's syntax, implemented from scratch in C11.

| | |
| --- | --- |
| Language name | CScript |
| Language id | `cscript` |
| File extension | `.cx` |
| TextMate scope | `source.cscript` |

## What it does

The grammar is derived from `docs/GRAMMAR.md` in the compiler repository, which
is what the parser actually implements — so nothing here highlights a form the
compiler would reject. It tells apart the things a JavaScript grammar would
flatten:

- **BigInt literals** from ordinary numbers, because they are a different type
  rather than a wider one — `42n` and `42` read differently.
- **Regular expressions** from division, so `a / b / c` is arithmetic.
- **Type annotations** — `: number`, `: bigint` — which are the only part of the
  syntax that is not JavaScript, and get a scope of their own rather than
  borrowing one.
- `#private` names, labelled statements and the `break label` that names one,
  `static` initialiser blocks, accessors including computed ones, `yield*`,
  `for await`, and `new.target`.

Also: comment toggling, bracket matching, auto-closing and surrounding pairs,
folding, indentation, and sixteen snippets.

## Installing

From a packaged build:

```bash
npx @vscode/vsce package          # produces cscript-language-0.1.0.vsix
code --install-extension cscript-language-0.1.0.vsix
```

For a quick look without packaging, copy this directory into
`~/.vscode/extensions/` and restart VS Code.

## Running it locally

```bash
cd editors/vscode-cscript
code .
```

Press **F5**. VS Code opens an *Extension Development Host* window with the
extension loaded. In it, open any `.cx` file — `tests/cases/language/` has a
hundred and twenty of them — or create one.

Check the **Language Mode** in the status bar. It should read **CScript**. If
it does not, the extension did not load; the Extension Development Host's
Output panel says why.

## Testing the grammar

The grammar is not taken on trust. Two checks run against the real corpus:

```bash
npm install                  # vscode-textmate and vscode-oniguruma
node test/tokenize.mjs $(find ../../tests/cases ../../examples -name '*.cx')
node test/scopes.mjs
```

The first tokenises every CScript program in the repository and reports any
text left unscoped — currently none, across 169 files and 34,534 tokens. The
second asserts that particular text carries a particular scope, which is what
catches a rule that matches but matches as the wrong thing: it is how
`new.target` was found being read as `new` followed by a property.

## The mark

A large C with the word set in its opening, the way C++ puts its `++` there.
The C is what survives being drawn at sixteen pixels beside a filename; the
word is what makes it this language rather than another one, and is legible
from about twenty-four pixels up. That is the trade a wordmark makes, and C++'s
own icon makes it too.

In the SVG the C is a stroked arc rather than a letter, so it renders the same
without depending on a font being installed, and the word carries `textLength`
so it cannot overflow the tile on a machine that has a different one.

## The file icon, and its one catch

VS Code takes the icon beside a filename from whichever **file icon theme** is
active, and an extension cannot add an icon to a theme it does not own. So
there are two ways to see the CScript mark:

**Use the theme this ships.** `Preferences: File Icon Theme` → *CScript (file
icons)*. It gives `.cx` the mark and leaves every other file to the defaults.

**Or keep your theme and add one mapping** in `settings.json`:

```jsonc
// Material Icon Theme
"material-icon-theme.files.associations": { "*.cx": "javascript" }

// vscode-icons
"vsicons.associations.files": [
  { "icon": "javascript", "extensions": ["cx"], "format": "svg" }
]
```

## Publishing

The manifest names the publisher `abbosbek-sulaymonov`. That publisher has to
exist on the Marketplace before `vsce publish` will work:

```bash
npx @vscode/vsce login abbosbek-sulaymonov    # or create-publisher
npx @vscode/vsce publish
```
