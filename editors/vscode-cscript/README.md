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

Braces around a C cut from a hexagon.

The hexagon is pointy-topped, which puts a flat edge on the right; dropping
that one edge opens the C by exactly sixty degrees. A wider mouth stops it
reading as a letter and starts it reading as a broken ring, which is why the
shape is built by removing an edge rather than a vertex.

Two things it deliberately does not depend on. The facets are drawn as separate
shapes rather than shaded, so nothing relies on gradient support; and the
braces are stroked curves rather than text, so nothing relies on a font being
installed. The vector and the raster were plotted from the same coordinates, so
they are the same picture rather than two drawings that resemble each other.

There are two brace colours, because a brace sits on whatever the editor's
background happens to be: a dark one vanishes on a dark theme and a light one
vanishes on a light theme. The faceted C is the same in both — it carries its
own colour.

Unlike a wordmark, this survives sixteen pixels: at the size a file icon
actually renders, it still reads as `{C}`.

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

The manifest names the publisher `abbosbek-sulaymonov`. That has to be a real
Marketplace publisher before anything can be published under it, and creating
one is the part that is not a command.

**1. An Azure DevOps organisation.** The Marketplace authenticates against Azure
DevOps, not GitHub. Sign in at <https://dev.azure.com> with a Microsoft account
and let it create an organisation; the name does not matter.

**2. The publisher.** At <https://marketplace.visualstudio.com/manage>, create a
publisher whose **ID** is exactly `abbosbek-sulaymonov` — the id, not the
display name, is what must match `package.json`. Change one and you change the
other.

**3. A personal access token.** In Azure DevOps: *User settings → Personal
access tokens → New token*.

| Field | Value |
| --- | --- |
| Organization | **All accessible organizations** |
| Scopes | *Custom defined* → **Marketplace → Manage** |
| Expiry | up to a year; it will need replacing |

The Organization field is the one that catches people. A token scoped to a
single organisation authenticates fine and then fails the publish with a bare
401, which reads like a bad token rather than a wrongly scoped one.

**4. Publish.**

```bash
cd editors/vscode-cscript
npx @vscode/vsce login abbosbek-sulaymonov   # paste the token once
npx @vscode/vsce publish                      # or: publish -p <token>
```

Later releases take a version argument instead of an edited manifest —
`npx @vscode/vsce publish minor` bumps `package.json`, packages and uploads in
one step.

### Before the first publish

```bash
npx @vscode/vsce package                       # must end with no warnings
code --install-extension cscript-language-0.1.0.vsix
```

Install the `.vsix` and open a `.cx` file. The status bar should read
**CScript**. That is the whole of what this extension does, and it is worth
seeing before it is public.

### Two things that are one-way

A published **version** cannot be withdrawn — only the whole extension can be
unpublished, and its name is then reserved and unusable. So the first publish
is worth doing from a `.vsix` you have actually installed.

The Marketplace takes a few minutes to show a new extension, and a little
longer before search finds it. Nothing has gone wrong in the meantime.

### VS Code forks

The Marketplace serves VS Code proper. VSCodium, Cursor, Gitpod and Eclipse
Theia pull from [Open VSX](https://open-vsx.org) instead, which is a separate
account and a separate upload:

```bash
npx ovsx publish cscript-language-0.1.0.vsix -p <open-vsx-token>
```

The same `.vsix` goes to both.
