/* jitcode.h — executable memory, and the arm64 encoder that fills it.
 *
 * Only the fully-typed numeric functions the IR can prove are compiled: every
 * operand of every arithmetic operation is known to be a number, so the code
 * has no type checks, no guards and nothing to deoptimise to. That is the
 * whole reason this design starts here rather than with a speculative
 * compiler — a JavaScript engine cannot know these things and CScript can.
 *
 * NaN-boxing is what makes the load path free. A number's Value *is* its
 * double, bit for bit, so a slot holding one is loaded straight into a
 * floating-point register: no tag test, no unboxing, no conversion.
 */
#ifndef CSCRIPT_JITCODE_H
#define CSCRIPT_JITCODE_H

#include "cscript/common.h"
#include "cscript/ir.h"
#include "cscript/value.h"

/* A compiled function.
 *
 * It is called with a pointer to the frame slots and a pointer to scratch
 * space for the IR's values, and returns the result's bits. Both arrays belong
 * to the caller, which is what keeps the compiled code free of any allocation
 * or stack management of its own. */
/* `exitTarget` is set to a bytecode offset when the code handed the frame back
 * to the interpreter rather than returning, and left alone otherwise — the
 * caller seeds it with -1 and reads it to tell the two apart. */
typedef uint64_t (*CompiledFn)(Value *slots, Value *scratch, int *exitTarget);

/* How many loop headers may get their own entry point. A function with more
 * loops than this is not the kind this backend is for. */
#define CS_JIT_MAX_OSR 8

/* An alternate entry point, for a loop that is already running.
 *
 * A function called once around a long loop is hot, but by the time the
 * counter says so the only call to it is already in progress — so compiling
 * for the call site is compiling for a call that never comes again. Without an
 * entry of this kind a loop benchmark never reaches the compiler at all, which
 * is exactly what the first measurements of this backend were quietly showing.
 *
 * On-stack replacement is unusually cheap here. The compiled code keeps locals
 * in the frame the interpreter gave it, at the same offsets, so handing over
 * mid-loop transfers no state: the entry re-reads the promoted slots and jumps
 * into the header. */
typedef struct {
  int bytecodeOffset; /* the loop header this entry stands for */
  CompiledFn entry;
} JitOsrEntry;

/* Where an exit leaves the operand stack, one per exit in the code. The VM
 * needs it because the compiled code writes the frame slots but not the
 * interpreter's `stackTop`, and a local in this VM *is* a stack slot. */
typedef struct {
  int bytecodeOffset;
  int stackHeight;
} JitExit;

typedef struct {
  CompiledFn entry;
  void *memory;
  size_t size;
  int scratchCount;

  JitOsrEntry osr[CS_JIT_MAX_OSR];
  int osrCount;

  /* Indexed by what the compiled code writes through `exitTarget`. */
  JitExit *exits;
  int exitCount;
} JitCode;

/* Compiles the IR, or returns NULL when it holds something the encoder does
 * not implement. The reason is written to `why`. */
JitCode *csJitCompile(const IrFunction *ir, const char **why);

void csJitCodeFree(JitCode *code);

#endif /* CSCRIPT_JITCODE_H */
