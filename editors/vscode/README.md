# CScript for VS Code

Syntax highlighting, bracket and comment behaviour, and a file icon for `.cx`.

## What you get without doing anything

Installing the extension gives `.cx` files the CScript grammar: keywords,
template literals with their interpolations, regular expressions told apart
from division, BigInt literals distinguished from ordinary numbers, `#private`
names, and CScript's type annotations — which are the one part of the syntax
that is not JavaScript and so get a scope of their own.

## The file icon, and its one catch

VS Code takes the icon beside a filename from whichever **file icon theme** is
active. An extension can ship a theme; it cannot add an icon to a theme it does
not own. So there are two ways to see the CScript mark:

**Use the theme this extension ships.** `Preferences: File Icon Theme` →
*CScript (file icons)*. It gives `.cx` the mark and leaves every other file to
VS Code's defaults.

**Or keep your own theme and add one mapping.** Both popular themes support
this in `settings.json`:

```jsonc
// Material Icon Theme
"material-icon-theme.files.associations": { "*.cx": "javascript" }

// vscode-icons
"vsicons.associations.files": [
  { "icon": "javascript", "extensions": ["cx"], "format": "svg" }
]
```

## Installing from source

```bash
cd editors/vscode
npx @vscode/vsce package     # produces cscript-0.1.0.vsix
code --install-extension cscript-0.1.0.vsix
```

Or, for a quick look: copy this directory into `~/.vscode/extensions/` and
restart VS Code.
