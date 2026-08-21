# Editor and tooling integrations

Kept here rather than in repositories of their own, because they are small and
they change when the grammar changes. Each is self-contained and could be split
out — `tree-sitter-cscript` will have to be, because Linguist vendors grammars
as submodules.

| Directory | What it is | State |
| --- | --- | --- |
| `vscode-cscript/` | VS Code extension: grammar, editing behaviour, snippets, file icon | Working; needs a Marketplace publisher to publish |
| `tree-sitter-cscript/` | tree-sitter grammar, for Neovim, Helix, Zed | Scaffold; parses 99 of 113 valid programs |
| `linguist/` | What a GitHub Linguist contribution needs | Prepared, **not submitted** |

## The mark

Braces around a C cut from a hexagon. Five images carry it: the icon and the
wordmark in `assets/`, the two file-icon variants the extension ships, and the
marketplace raster. They share one geometry, so a change to the shape is a
change to all five.

## The identifiers, which are the same everywhere

```
Name             CScript
Language id      cscript
File extension   .cx
TextMate scope   source.cscript
```

`.cx` rather than `.cscript`: it is what the repository, the documentation and
a hundred and twenty-seven test cases already use, and the runtime never
inspects an extension anyway — module resolution takes a specifier as written,
so the name of the file is a convention rather than a rule.

## What is true on GitHub today

`.gitattributes` maps `.cx` to JavaScript, which makes files highlight and the
repository count as what it mostly is. GitHub does **not** know the name
CScript, and will not until a Linguist contribution is accepted —
`linguist/README.md` says what that needs and why the bar is not met yet.
