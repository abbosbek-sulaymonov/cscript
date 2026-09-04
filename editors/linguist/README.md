# Getting CScript recognised by GitHub

**Nothing in this directory changes anything on github.com yet.** It is the
material a Linguist contribution needs, prepared so that submitting it is a
matter of following the process rather than starting one.

## What is and is not true today

| Claim | Status |
| --- | --- |
| `.cx` highlights on GitHub | **Yes** — `.gitattributes` maps it to JavaScript |
| The repository counts as a language | **Yes**, as JavaScript |
| GitHub says "CScript" | **No** — needs a Linguist contribution |
| A CScript icon in GitHub's file browser | **No** — follows from the above |

`.gitattributes` at the repository root carries:

```gitattributes
*.cx linguist-language=JavaScript
```

That works because Linguist already knows JavaScript. `linguist-language=CScript`
would *not* work, because Linguist has never heard of CScript — a repository
cannot invent a language for itself.

## What a contribution requires

Linguist's own
[adding-a-language](https://github.com/github-linguist/linguist/blob/main/docs/adding-a-language.md)
document is the authority. Its bar, in short:

1. **Use in the wild.** Hundreds of repositories, owned by unrelated people,
   using the extension. This is the criterion CScript does not meet, and no
   amount of preparation substitutes for it.
2. **A grammar** with an open-source licence, submitted as a submodule under
   `vendor/grammars/`. The one in `editors/vscode-cscript/syntaxes/` is that
   grammar; it would need its own repository.
3. **An entry in `languages.yml`.** Below.
4. **Samples** under `samples/CScript/`, which the repository has no shortage
   of — `tests/cases/` and `examples/`.

## The `languages.yml` entry

```yaml
CScript:
  type: programming
  color: "#2b6cb0"
  extensions:
    - ".cx"
  tm_scope: source.cscript
  ace_mode: javascript
  codemirror_mode: javascript
  codemirror_mime_type: text/javascript
  language_id: 
```

Two fields deliberately left to be filled in at submission time:

- **`language_id`** is assigned by Linguist and must be unique. It is generated
  by their tooling, not chosen.
- **`.cx`** must be checked against every language already claiming it before
  submitting. If it collides, the entry needs `group` or a disambiguation
  heuristic in `heuristics.yml`, and the pull request has to say so.

`ace_mode` and `codemirror_mode` are JavaScript's on purpose: those editors have
no CScript mode, and JavaScript's is right for everything but the type
annotations.

## Order of work

The extension and this are independent, and only one of them is in anyone's
hands but ours:

1. Publish the VS Code extension. (Done here; needs a Marketplace publisher.)
2. Split the grammar into its own repository, since Linguist vendors grammars
   as submodules.
3. Wait for adoption. This is the real gate.
4. Open the Linguist pull request with the entry above, the grammar submodule
   and the samples.

Until step 4 is merged, GitHub does not recognise CScript, and saying otherwise
in a README would be a claim about someone else's software.
