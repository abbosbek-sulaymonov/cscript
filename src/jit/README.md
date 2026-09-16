# `src/jit/` — the second tier

What happens to a function that runs enough to earn it:

```
bytecode ──▶ tiering ──▶ typed IR ──▶ passes ──▶ arm64
             jit.c       ir*.c                   jitcode*.c
                            │
                            └─▶ IR interpreter (make test-ir)
```

A function crossing the threshold is considered **once**, in
[`jit.c`](jit.c): its bytecode is scanned for anything the backend would
refuse, it is lowered to the typed IR, the IR's passes run, and where every
arithmetic operand is *proved* a number it is compiled to machine code. Each of
those can decline, and a refusal is recorded rather than discarded —
`--jit-report` prints what happened to every hot function.

The thing this tier has that a JavaScript engine cannot: **declared types**.
Where the checker proved the arithmetic, the compiled code carries no type
tests and has nothing to deoptimise to.

## The files

| Tiering | Holds |
| --- | --- |
| [`jit.c`](jit.c) | What gets hot, what happens to it, and what holds the result |

| Lowering to the IR | Holds |
| --- | --- |
| [`ir.c`](ir.c) | The walk: an abstract interpretation of the bytecode, keeping a compile-time model of the operand stack |
| [`ir_build.c`](ir_build.c) | Registers, instructions, block boundaries, and the push and pop the walk rests on |
| [`ir_lower_data.c`](ir_lower_data.c) | Constants, globals, locals, the operand stack |
| [`ir_lower_arith.c`](ir_lower_arith.c) | Arithmetic and comparison, on operands proved to be numbers |
| [`ir_lower_object.c`](ir_lower_object.c) | Property reads and writes, and the layouts they assume |
| [`ir_allocate.c`](ir_allocate.c) | Building an object from compiled code, and the slots that involves |
| [`ir_lower_flow.c`](ir_lower_flow.c) | Jumps, branches, calls and returns |
| [`ir_inline.c`](ir_inline.c) | Splicing a small callee's body in where the call was |
| [`ir_replay.c`](ir_replay.c) | What the interpreter does across a hand-over, modelled rather than run |
| [`ir_types.c`](ir_types.c) | The passes that decide what the lowered form is worth |
| [`ir_interpret.c`](ir_interpret.c) | Running the lowered form, to check it against the bytecode |
| [`ir_print.c`](ir_print.c) | The IR in readable form, and where its typing stops |
| [`ir_internal.h`](ir_internal.h) | The lowering's own state, and the seams between all of those |

| The arm64 backend | Holds |
| --- | --- |
| [`jitcode.c`](jitcode.c) | Compiling a lowered function, and the memory it runs from |
| [`jitcode_emit.c`](jitcode_emit.c) | One IR instruction to machine code — a switch and nothing else |
| [`jitcode_arm64.c`](jitcode_arm64.c) | The handful of arm64 instructions this needs, each written out |
| [`jitcode_alloc.c`](jitcode_alloc.c) | Deciding where every value lives |
| [`jitcode_internal.h`](jitcode_internal.h) | The buffer, the registers, the homes, and `EmitAt` |

The public shapes are [`../../include/cscript/ir.h`](../../include/cscript/ir.h)
and [`../../include/cscript/jitcode.h`](../../include/cscript/jitcode.h); the
two internal headers are the seams between the files that *produce* one.
`jitcode_internal.h` is arm64-specific throughout — a second backend would
want its own, behind the same IR.

## How the four lowering groups fit together

The instruction walk asks each group in turn, and each keeps the `switch` its
cases were written in. A group answers:

| `LowerResult` | Means |
| --- | --- |
| `LOWER_OK` | Emitted; carry on with the next instruction |
| `LOWER_HAND_OVER` | The interpreter takes the frame from here |
| `LOWER_FAILED` | The whole function is refused, and `low->reason` says why |
| `LOWER_UNHANDLED` | Another group's opcode — ask the next one |

Everything a case needs is in the `LowerAt` context rather than in the locals
of the function driving the walk, which is what let the case bodies move
between files unchanged. Adding an opcode to the compiler means a case in
whichever group owns its role, and then a case in
[`jitcode_emit.c`](jitcode_emit.c).

## How any of this is known to be correct

A golden file pins what a program prints. It says nothing about whether two
execution paths agree, so the compiler is verified by making them disagree
visibly:

| Suite | What it does |
| --- | --- |
| `make test-ir` | Runs the **IR interpreter in place of the bytecode** for every function the lowering accepts. A mistranslation becomes a failing golden test |
| `make test-jit` | Runs every program twice in one binary — compiler off, then on — and requires the two to agree, reporting how many calls, loops and exits the compiler actually took |
| `make test-jit-gc` | The two hardest configurations at once: compiled code plus a collection at every allocation |
| `bench/jit.sh` | What it is worth, with the compiler on and off |

The coverage counters matter as much as the pass: a sweep that agrees on
programs the compiler declined to touch is not evidence of anything.

## Related

- [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md#the-typed-ir) — the whole story in order, stage by stage, including the five soundness bugs and what found them
- [../../docs/ROADMAP.md](../../docs/ROADMAP.md) — what the measurements say to build next
- `CS_JIT_DUMP_IR=1` prints the lowered form; `--jit-threshold <n>` makes a test case hot in one run
