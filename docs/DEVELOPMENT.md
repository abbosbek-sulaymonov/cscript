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
make test-gc      # same suite, collecting on every allocation
make test-switch  # same suite, forcing the portable switch dispatch
make test-tagged  # same suite, forcing the 16-byte tagged-union Value
make test-node    # examples must match Node.js output
make test-all     # all five
make test FILTER=scoping
UPDATE=1 tests/run_tests.sh    # rewrite .expected from actual output
```

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

> AddressSanitizer fails to start under some sandboxed environments, which is
> why `make test` uses the UBSan build and ASan is a separate target.

---

## Project layout

```
cscript/
├── include/cscript/     one public header per module
├── src/
│   ├── lexer.c          source text  -> tokens
│   ├── parser.c         tokens       -> AST   (precedence climbing, recovery)
│   ├── ast.c            node constructors and the arena
│   ├── compiler.c       AST          -> bytecode (scope resolution lives here)
│   ├── vm.c             the interpreter loop
│   ├── native.c         console, Math, and the conversion functions
│   ├── memory.c         allocator and mark-sweep collector
│   ├── object.c         heap object types, string interning
│   ├── table.c          open-addressing hash table
│   ├── value.c          value operations, coercion, formatting
│   ├── chunk.c          bytecode buffer and constant pool
│   ├── debug.c          disassembler and AST printer
│   ├── diagnostic.c     error reporting with source spans
│   └── main.c           CLI, REPL, file runner
├── tests/
│   ├── run_tests.sh     golden-file runner
│   ├── node_parity.sh   examples vs. Node.js
│   └── cases/           *.cx paired with *.expected
├── examples/            runnable sample programs
└── docs/
    ├── ARCHITECTURE.md  how the pipeline and the GC work
    └── GRAMMAR.md       full grammar, semantics, and every fix to JavaScript
```

---

---

## Editor integrations

`editors/` holds the VS Code extension, the tree-sitter grammar and the
material a GitHub Linguist contribution needs. Each has a README of its own;
[editors/README.md](../editors/README.md) says which is in what state.
