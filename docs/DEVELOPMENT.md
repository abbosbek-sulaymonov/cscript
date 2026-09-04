# Working on CScript

How to build it, how it is tested, and where everything lives.

```bash
make            # optimised build       -> build/release/cscript
make debug      # -O0 -g3 + UBSan       -> build/debug/cscript
make asan       # + AddressSanitizer
make gcstress   # collects on every allocation
make trace      # debug + dumps every stage
make clean
make help
```

Each configuration builds into its own directory, so switching between them
never leaves a stale object behind.

### Tests

```bash
make test         # golden-file suite under UBSan — UB aborts the run
make test-asan    # same suite under AddressSanitizer
make test-gc      # same suite, collecting on every allocation
make test-switch  # same suite, forcing the portable switch dispatch
make test-tagged  # same suite, forcing the 16-byte tagged-union Value
make test-ir      # same suite with the IR *replacing* the interpreter
make test-jit     # compiled and interpreted must print the same thing
make test-jit-gc  # the same, with a collection on every allocation
make test-node    # examples must match Node.js output
make test-regex   # the regex engine on its own, without the language
make test-all     # the seven that make up a clean run
make test FILTER=scoping
UPDATE=1 tests/run_tests.sh    # rewrite .expected from actual output
```

`make test-all` is `test`, `test-gc`, `test-switch`, `test-tagged`,
`test-node`, `test-ir` and `test-jit`. `test-asan` is deliberately not among
them — see the note below — `test-regex` needs no language build, and
`test-jit-gc` is slow enough to be run deliberately rather than always.

Every `tests/cases/<group>/NAME.cx` runs and its output is compared against
`NAME.expected`; cases named `error_*` assert the failure path.

**`make test-gc` is not decoration.** Collecting on every single allocation
means any value the collector cannot reach from a root is freed the instant it
becomes unreachable. It caught a real bug in milestone 1 — the executing chunk's
constant pool was never marked, so string literals were collected mid-run.

**`make test-node` is what keeps the headline claim true.** It runs each example
under Node and requires byte-identical output, except for the files listed as
deliberately divergent — which it requires to actually differ, so a fix that
silently stops working also fails the build.

**`make test-ir` and `make test-jit` are how the second tier is checked.** The
first raises the compiler's threshold to one and runs the whole golden suite
with the lowered IR *replacing* the interpreter wherever it reaches, so a
mistranslation fails a test rather than producing a quietly wrong number. The
second runs every program twice in the same binary — compiler off, compiler on
— and requires the two to agree byte for byte, which is the check a golden file
cannot make. It also reports how much the compiler took: a drop in that is a
coverage regression even when nothing disagrees.

`make test-jit-gc` runs that second check against a build that *also* collects
on every allocation, which is the only place the two meet. Compiled code holds
references nothing else does — the shapes its property accesses were lowered
against, and the closures whose bodies were spliced into it — and a missing
root there is invisible until a collection happens to land between the
assumption being recorded and the entry that checks it.

> AddressSanitizer does not always start. On some macOS builds its runtime
> deadlocks inside `AsanInitInternal` — spinning on its own mutex before `main`
> is reached, so nothing of the program runs — and under some sandboxes it
> fails outright. Either would wedge a suite it was part of, which is why
> `make test` uses the UBSan build and `test-asan` is a separate target outside
> `test-all`. Run it when you are hunting a memory error, and expect to retry.

---

## Project layout

Sources are grouped by role rather than by phase, because the pieces of a
recursive-descent parser are mutually recursive and layering them would have
been a fiction. [ARCHITECTURE.md](ARCHITECTURE.md#source-layout) lists every
file; this is the shape.

```
cscript/
├── include/cscript/     one public header per subsystem
├── src/
│   ├── compiler/        lexer, parser, type checker, bytecode compiler
│   ├── runtime/         the VM, the object model, the collector, modules
│   ├── native/          the standard library, one file per type
│   ├── jit/             tiering, the typed IR, the arm64 encoder
│   └── main.c           CLI, REPL, file runner
├── tests/
│   ├── run_tests.sh     golden-file runner
│   ├── node_parity.sh   examples vs. Node.js
│   ├── jit_differential.sh  compiled and interpreted must agree
│   ├── regex_engine_test.c  the regex engine on its own
│   └── cases/           *.cx paired with *.expected, grouped by role:
│                        language, library, types, async, imports, errors, jit
├── examples/            runnable sample programs
├── bench/               the benchmarks, and the Rust and C ports
├── editors/             VS Code, tree-sitter, and the Linguist material
└── docs/                this directory
```

---

---

## Benchmarks

`bench/run.sh` is CScript against Node, `bench/jit.sh` is the compiler against
the interpreter in the same binary, and `bench/rust/run.sh` is CScript against
Rust on the three benchmarks that have a port. The Rust one needs `rustc` and
skips nothing quietly — it rebuilds the ports every time.

---

## Editor integrations

`editors/` holds the VS Code extension, the tree-sitter grammar and the
material a GitHub Linguist contribution needs. Each has a README of its own;
[editors/README.md](../editors/README.md) says which is in what state.
