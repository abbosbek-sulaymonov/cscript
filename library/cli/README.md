# `std:cli`

A command line, described once and parsed from that description.

```ts
import { parse, help } from "std:cli";
import { args, exit } from "std:os";

const spec = { name: "notes", version: "1.0", flags: [
  { name: "output", short: "o", type: "string", fallback: "-" },
] };

const parsed = parse(args(), spec);
if (parsed.isErr) { console.log(parsed.error); console.log(help(spec)); exit(64); }
```

`os.args()` hands back an array of strings. Everything after that — long flags,
short ones, `--flag=value`, bundled booleans, `--`, the difference between a
missing value and an empty one, and a help text that matches what the program
actually accepts — is the same two hundred lines in every program ever written.

## Specification

### The description

```ts
{
  name: "notes",
  version: "1.2.0",
  description: "Keep short notes.",
  positional: "[file...]",          // shown in the usage line
  flags: [
    { name: "output", short: "o", type: "string", fallback: "-",
      description: "where to write", required: false },
    { name: "format", type: "string", oneOf: ["text", "json"] },
  ],
  commands: [
    { name: "add", description: "add a note", flags: [ … ] },
  ],
}
```

`type` is `"boolean"`, `"string"`, `"number"` or `"list"` — a list may be given
more than once and collects. `oneOf` restricts a value to a set. A boolean
needs no value and defaults to false.

### Parsing

| Export | Signature | Answers |
| --- | --- | --- |
| `parse` | `(args, spec)` | `Result` of the parse, or of a message saying what was wrong |
| `help` | `(spec, forCommand?)` | the help text, generated from the same description |
| `version` | `(spec)` | `"notes 1.2.0"` |

A successful parse answers `{ options, positional, command, commandOptions,
commandPositional, wantsHelp, wantsVersion }`.

### What it accepts

| Written | Means |
| --- | --- |
| `--output x`, `--output=x` | a long flag with a value |
| `-o x`, `-ox` | the same, short |
| `-abc` | three booleans, or booleans then a value flag that ends the bundle |
| `--no-colour` | turns off a boolean whose default is true |
| `--` | everything after it is positional, whatever it looks like |
| `-` | a positional, by long convention: standard input |
| `add --pin` | a command, then its **own** flags |

## Implementation

**Every flag is present at its fallback** after a successful parse, so a caller
never reads a key that is not there — which in CScript is a runtime error
rather than `undefined`.

**`wantsHelp` and `wantsVersion` are answered, not acted on.** Printing and
exiting is the program's decision, and a parser that called `exit` could not be
tested. `--help` is also allowed to skip a required flag, since asking for help
is not running the command.

**Failures are values.** A command line is input from outside; a program should
print what was wrong with it rather than stopping at a runtime error.

**Help is generated from the description the parser uses**, which is the whole
reason the description is data: the two cannot drift.

**A command's flags come from its own description**, because `serve --port` and
`build --port` need not mean the same thing.

**A program takes a command or takes files, not both.** With commands in the
description, a bare word is a command and an unknown one is an error rather
than a filename nobody noticed.

**Deliberately absent:** environment-variable fallbacks, config-file merging,
coloured output. Each belongs to the program rather than the parser, and each
is what turns an argument parser into a framework.

## Source

[`cli.cx`](cli.cx). `walk`, `takeLong`, `takeShort` and `storeValue` are
private.
