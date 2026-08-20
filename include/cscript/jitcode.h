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
typedef uint64_t (*CompiledFn)(Value *slots, Value *scratch);

typedef struct {
  CompiledFn entry;
  void *memory;
  size_t size;
  int scratchCount;
} JitCode;

/* Compiles the IR, or returns NULL when it holds something the encoder does
 * not implement. The reason is written to `why`. */
JitCode *csJitCompile(const IrFunction *ir, const char **why);

void csJitCodeFree(JitCode *code);

#endif /* CSCRIPT_JITCODE_H */
