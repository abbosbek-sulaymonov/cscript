# `src/` — the implementation

Four groups and an entry point. Every public header is in
[`include/cscript/`](../include/cscript); everything here is what implements
one.

| Directory | Holds | Guide |
| --- | --- | --- |
| [`compiler/`](compiler) | Source text to bytecode: lexer, parser, tree, type checker, code generator, disassembler | [compiler/README.md](compiler/README.md) |
| [`runtime/`](runtime) | The VM that runs the bytecode: values, objects, memory, modules, the regex engine | [runtime/README.md](runtime/README.md) |
| [`native/`](native) | The standard library — one namespace per file | [native/README.md](native/README.md) |
| [`jit/`](jit) | The second tier: tiering, the typed IR, the arm64 backend | [jit/README.md](jit/README.md) |
| [`main.c`](main.c) | Option parsing, the REPL, and the exit codes | [../docs/CLI.md](../docs/CLI.md) |

```
source ──▶ lexer ──▶ parser ──▶ type checker ──▶ compiler ──▶ VM
                       │             │              │          │
                      AST      annotated AST     bytecode    values
                                                                │
                                          hot ─▶ typed IR ─▶ arm64
```

The grouping is by role rather than by stage, which is why `native/` is beside
`runtime/` rather than inside it: a built-in is written against the VM's public
surface, and keeping the two apart is what stops that surface from quietly
growing.

For *why* any of it is shaped this way — the measurements, and the
optimisations that returned nothing — read
[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md). This file and the four beside
it only say where things are.

## The conventions every file here follows

**A header comment that says what the file is for, and why it is a file.** The
first thing in every `.c`, `.h` and `.inc` under `src/` is a block naming the
file and the one thing it holds. Where a file exists because of a measurement
or a hazard, the block says which — that is the part a reader cannot recover
from the code.

**One role per file, 500–600 lines at most.** A file that outgrows that is
split along a seam that already existed: the group of opcodes it handled, the
kind of node it compiled, the phase it belonged to. Nothing is split merely to
land under the limit — `ir_lower_*.c` are four files because the lowering asks
four questions, and the size limit is what made that visible.

**Cross-file `static`s become a named export.** A helper that has to be shared
after a split gets a project prefix — `csNativeAppendRooted`,
`csIrLowerArith`, `csValueFormatNumberEx` — and is declared in an internal
header, never in a public one.

**`*_internal.h` is the seam, and only for its own group.** Each group of
files that used to be one file keeps a header holding exactly what crosses
between them; every one says in its own comment that nothing outside the group
includes it. The public shape stays in `include/cscript/`, so a reader can tell
an implementation detail from an interface by which directory the header is in.

**Includes are paths from `src/`.** A file names another group's internal
header as `runtime/vm_internal.h`, not `../runtime/vm_internal.h`, so the
direction of a dependency is visible at the include rather than in a chain of
dots. `-Isrc` in the Makefile is what makes that work.

**`.inc` files are C source, not headers.** They exist only in `runtime/`, for
the interpreter, and only because measurement forced it — see
[runtime/README.md](runtime/README.md#the-interpreter-is-one-translation-unit).
Nothing else in the project uses the technique.

## Adding to it

| I want to… | Start at |
| --- | --- |
| add a syntax form | [`compiler/`](compiler/README.md) — parser first, then the code generator |
| add an opcode | [`../include/cscript/opcode.h`](../include/cscript/opcode.h) — the X-macro generates the enum *and* the dispatch table, so they cannot drift |
| add a built-in | [`native/`](native/README.md) — a new namespace is a new file that installs itself |
| add a fast path to the interpreter | [`runtime/`](runtime/README.md) — the opcode bodies are the `vm_ops_*.inc` set |
| teach the compiler a new opcode | [`jit/`](jit/README.md) — one of the four `ir_lower_*.c`, then `jitcode_emit.c` |
| find out why a change made things slower | `make profile` for the opcode tables, `bench/jit.sh` for the compiler |
