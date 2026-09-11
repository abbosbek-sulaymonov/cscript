# The command line

What `cscript` does with its own arguments, and what the REPL does with yours.

```
cscript                     start the REPL
cscript [options] <file.cx> [args...]
cscript [options] -e <code>
cscript [options] -         read the program from stdin
```

Options come before the program. `--` ends them, so a script whose name starts
with a dash can still be run, and everything after the script belongs to the
script rather than to `cscript`.

---

## Options

| Option | What it does |
| --- | --- |
| `-e`, `--eval <code>` | Run `<code>` as a program. `--eval=<code>` also works |
| `-c`, `--check` | Compile and type-check, and stop there |
| `--print-tokens` | Dump the token stream of every file read |
| `--print-ast` | Dump the parse tree, with the types the checker resolved |
| `--print-bytecode` | Disassemble each compiled chunk |
| `--time` | Report how long the program took, on stderr |
| `--jit-threshold <n>` | How much work a function does before it is compiled |
| `--no-jit` | Never tier up, whatever gets hot |
| `--jit-report` | Print what the compiler took, on exit |
| `-h`, `--help` | Usage |
| `-v`, `--version` | The version, and nothing else |

## Exit codes

Following sysexits, which is what a shell script checking one expects.

| Code | Means |
| --- | --- |
| 0 | The program ran |
| 64 | The command line was wrong |
| 65 | The program would not compile |
| 66 | The file was not there, or could not be read |
| 70 | The program failed while running |

66 rather than 65 for a missing file is the distinction worth having: a file
that is not there is not a program with a mistake in it, and a build script
checking the difference should not have to read the message to tell.

---

## `--check`

Runs the parser, the type checker and the code generator, and stops before the
program does anything.

```bash
cscript --check src/main.cx     # 0, or 65 with the errors on stderr
```

Every stage has something to say about a program before it runs, and an editor
or a CI step wants to hear all of it without the side effects. Imports are
resolved and compiled too, so a broken one is reported here rather than at the
first statement that needs it:

```
$ cscript --check main.cx
lib.cx:3: error: cannot assign string to 'total', declared as number
```

## Printing the stages

Any stage of the pipeline, for any program, in any build:

```bash
cscript --print-tokens   -e 'let a = 1;'
cscript --print-ast      -e 'let a = 1;'
cscript --print-bytecode examples/fizzbuzz.cx
```

These used to be compile-time only — `make trace` turned all of them on for
the whole run, and nothing else could turn any of them on at all, so looking at
one function's bytecode meant rebuilding the interpreter. The printers were
always in every build; it was the *asking* that was not. `make trace` still
starts with every stage on, which is what it was for.

`--print-bytecode` covers every file the program is made of, not only the one
named on the command line, because a program with imports is several chunks and
the interesting one is rarely the entry point.

## Arguments

Anything after the script is the script's:

```bash
$ cat args.cx
console.log(process.argv.length, process.argv[2]);
$ cscript args.cx --print-ast
3 --print-ast
```

`process.argv` has Node's shape — the executable, then the script, then the
arguments — because a program that reads it then runs under both, which is the
claim the whole test suite is built to keep. `-e` and the REPL have no script,
and Node leaves that slot out as well.

It is the only member of `process` there is. It exists because a script given
arguments has to be able to read them, not as the start of a Node-compatible
runtime surface.

## The standard library

`import { range } from "std:iter"` resolves against the **binary's own
location** rather than the program's: `<binary>/../lib/cscript` for an
installed tree, `<binary>/../../library` for a source checkout, then
`<binary>/library`.

`CSCRIPT_STD_PATH` overrides all of that with a directory of your own. Setting
it to somewhere that is not a directory is an error rather than a reason to
fall back — falling back would run a library the environment said not to use.

```bash
CSCRIPT_STD_PATH=./library ./build/release/cscript program.cx
```

The error when a module cannot be found says which of the two things went
wrong: no library at all, or no module of that name in the one that was found.
[../library/README.md](../library/README.md) lists the modules.

---

## The compiler's flags

`--jit-threshold` and `--no-jit` do what `CS_JIT_THRESHOLD` in the environment
does, and the flag wins where both are given. `--no-jit` puts the threshold out
of reach rather than adding a branch to the counter, which is why it costs
nothing to have.

```bash
cscript --jit-threshold 100 --jit-report bench/jit/jit_loop.cx
```

The tiering counter is built into the `jit` configuration only, so these are
accepted everywhere and have something to do in that build. See
[PERFORMANCE.md](PERFORMANCE.md) for what the compiler is worth and
[DEVELOPMENT.md](DEVELOPMENT.md) for how to build it.

---

## The REPL

```
$ cscript
cscript 0.45.0 — .help for help, Ctrl-D to exit
> 1 + 1
2
> let people = ["Ada", "Alan"]
> people.map(n => n.length)
[ 3, 4 ]
> function twice(x) {
...   return x * 2;
... }
> twice(21)
42
```

**An entry that is one expression has its value printed.** Whether it is one is
asked of the parser rather than guessed at from the first word, because the
answer decides what happens to the entry and a guess would be wrong on exactly
the entries people type. A declaration prints nothing, as in Node; a
`console.log(…)` prints its argument and then `undefined`, also as in Node,
because the call is itself an expression whose value is `undefined`.

**A missing semicolon is added.** CScript has [no automatic semicolon
insertion](GRAMMAR.md#7-no-automatic-semicolon-insertion) and a file needs its
semicolons — but an entry at a prompt is a fragment rather than a file, and one
is added when that is all the entry needs to parse. The entry is tried as typed
first, so nothing is changed about a program that was already complete.

**An entry with a bracket open keeps reading**, and `...` asks for the rest of
it. Counted with the real lexer rather than by scanning for braces, because a
brace inside a string or a comment closes nothing and a template literal
contains both.

| Command | What it does |
| --- | --- |
| `.help` | The above, briefly |
| `.exit` | Leave. So does Ctrl-D |

Bindings persist between entries, because each entry is compiled against the
same module: `let x = 5` and then `x * 2` works, and so does redeclaring `x`,
which a file would refuse.

There is no line editing and no history — that needs a terminal library, and
the REPL is not where the interesting part of this project is. `rlwrap cscript`
gets both.
