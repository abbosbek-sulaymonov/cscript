/* jitcode_internal.h — what the backend's files share.
 *
 * The public shape of a compiled function is in include/cscript/jitcode.h.
 * This is the seam between the pieces that produce one: the buffer being
 * filled, the arm64 instructions it can be filled with, where each value has
 * been put, and the one thing an instruction's emission needs to know.
 *
 * Nothing outside src/jit/jitcode*.c includes this. It is arm64-specific
 * throughout: a second backend would want its own, behind the same IR.
 */
#ifndef CSCRIPT_JIT_JITCODE_INTERNAL_H
#define CSCRIPT_JIT_JITCODE_INTERNAL_H

#include "cscript/common.h"
#include "cscript/ir.h"
#include "cscript/jitcode.h"

/* ---- the buffer --------------------------------------------------------- */

typedef struct {
  uint32_t *words;
  int count;
  int capacity;
  bool failed;
} Encoder;

void csJitWord(Encoder *encoder, uint32_t instruction);

/* ---- where things live -------------------------------------------------- */

/* x19–x21 hold the three arguments and x22 upwards the address of one global
 * each, all materialised on entry. They are callee-saved on purpose: a
 * function that calls out — `%` does — must not have them clobbered, which is
 * what makes calling out cost nothing around the call. */
#define REG_SLOTS 19
#define REG_SCRATCH 20
#define REG_EXIT 21 /* where the third argument was put */
#define REG_FIRST_GLOBAL 22
#define REG_TEMP 9

/* d2, free in either pool, for the one argument arrangement that is a genuine
 * swap and cannot be done with two moves. */
#define CALL_SHUFFLE 2

/* x28 holds the address of the one function this backend calls, materialised
 * on entry rather than at every call — four instructions an iteration for a
 * value that never changes is the same waste hoisting the constants fixed. */
#define REG_CALL_TARGET 28

/* d0 and d1 stay free as scratch for operands that did not get a register. */
#define ALLOC_FIRST_SCRATCH 0
#define ALLOC_SECOND_SCRATCH 1
#define ALLOC_POOL_SIZE 22

/* A function that calls out allocates only from d8–d15: the registers a call
 * is obliged to preserve, so nothing live has to be spilled around one. It
 * costs fourteen registers, and a loop that fits in eight is most of them. */
#define ALLOC_CALLSAFE_POOL_SIZE 8

/* arm64 condition codes for the comparisons, on the *ordered* forms — an
 * unordered compare (either side NaN) must not take the branch, which is what
 * distinguishes MI/GT/LS from LT/GE and matters for `x < NaN`. */
#define COND_MI 0x4u /* less than, ordered */
#define COND_LS 0x9u /* less or equal, ordered */
#define COND_GT 0xcu
#define COND_GE 0xau
#define COND_EQ 0x0u
#define COND_NE 0x1u

/* A branch whose target block is not laid out yet. */
typedef struct {
  int at;    /* index of the instruction to patch */
  int block; /* where it should go */
  bool conditional;
  uint32_t condition;
} Fixup;

/* ---- the arm64 instructions this needs ---------------------------------- */

void csJitLdrDouble(Encoder *e, int destination, int base, int byteOffset);
void csJitStrDouble(Encoder *e, int source, int base, int byteOffset);
void csJitLdrGeneral(Encoder *e, int destination, int base, int byteOffset);
void csJitStrGeneral(Encoder *e, int source, int base, int byteOffset);
void csJitStrWord32(Encoder *e, int source, int base);

void csJitFadd(Encoder *e, int d, int n, int m);
void csJitFsub(Encoder *e, int d, int n, int m);
void csJitFmul(Encoder *e, int d, int n, int m);
void csJitFdiv(Encoder *e, int d, int n, int m);
void csJitFneg(Encoder *e, int d, int n);
void csJitFcmp(Encoder *e, int n, int m);
void csJitFmovDouble(Encoder *e, int d, int n);
void csJitFmovToDouble(Encoder *e, int d, int n);
void csJitFmovToGeneral(Encoder *e, int d, int n);

void csJitMovImmediate(Encoder *e, int destination, uint64_t value);
void csJitMovRegister(Encoder *e, int destination, int source);
void csJitAndRegisters(Encoder *e, int d, int n, int m);
void csJitCmpGeneral(Encoder *e, int n, int m);
void csJitCsel(Encoder *e, int d, int n, int m, uint32_t condition);
void csJitFcvtzs(Encoder *e, int d, int n);
void csJitScvtf(Encoder *e, int d, int n);
void csJitSdiv(Encoder *e, int d, int n, int m);
void csJitMsub(Encoder *e, int d, int n, int m, int a);

/* Placeholders, patched once their target is known. Each answers where it put
 * the instruction so the caller can come back to it. */
int csJitCbzHere(Encoder *e, int t);
int csJitBranchHere(Encoder *e, uint32_t condition);
int csJitJumpHere(Encoder *e);
void csJitPatchToHere(Encoder *e, int at);

void csJitEmitPrologue(Encoder *e);
void csJitEmitEpilogue(Encoder *e);

/* ---- deciding where each value lives ------------------------------------ */

uint32_t csJitConditionFor(IrOp op);
int csJitPoolSize(bool callSafe);
int csJitAllocPool(int index, bool callSafe);

/* Which frame slots can live in a register for the whole function, and where
 * each IR value ends up. -1 in the second means the scratch array. */
bool *csJitPromotableSlots(const IrFunction *ir);
int *csJitAllocateRegisters(const IrFunction *ir, int reserved, bool callSafe);

/* The register holding a value: its own, or a scratch one it is loaded into. */
int csJitReadOperand(Encoder *encoder, const int *home, int value, int scratch);

/* Whether every block can be entered with nothing live in a register, which is
 * what makes on-stack replacement a jump rather than a translation. */
bool csJitBlocksAreSelfContained(const IrFunction *ir);

/* A block reached by an edge that does not move forward is a loop header. */
bool csJitIsLoopHeader(const IrFunction *ir, int target);

/* ---- emitting one instruction ------------------------------------------- */

/* Everything the emission of a single instruction needs.
 *
 * The two growable arrays are held by pointer because a case may append to
 * them: an exit records where the interpreter picks up, and a branch records
 * a patch to make once its target block is laid out. `index` is a pointer for
 * the same reason — a comparison immediately followed by a branch consumes
 * both, and says so by advancing it. */
typedef struct {
  Encoder *encoder;
  const IrFunction *ir;
  const IrInst *inst;

  const int *home;     /* IR value -> register, or -1 for the scratch array */
  const int *slotHome; /* frame slot -> register, or -1 for memory */
  const uint64_t *constantValue;
  const int *constantHome;
  int constantCount;
  const int *globalName;
  int globalCount;

  JitExit **exits;
  int *exitCount;
  int *exitCapacity;
  Fixup **fixups;
  int *fixupCount;
  int *fixupCapacity;

  int block;   /* the block being emitted */
  int *index;  /* the instruction's position in it */
  const char **why;
} EmitAt;

/* Records a branch to a block that is not laid out yet. */
void csJitAddFixup(EmitAt *at, int instructionAt, int block, bool conditional,
                   uint32_t condition);

/* Records where the interpreter picks the frame up, and how deep its operand
 * stack is there. Returns the index the compiled code writes back. */
int csJitAddExit(EmitAt *at, int bytecodeOffset, int stackHeight);

/* Emits one instruction. False when the encoder cannot express it, with the
 * reason written through `at->why`. */
bool csJitEmitInstruction(EmitAt *at);

#endif /* CSCRIPT_JIT_JITCODE_INTERNAL_H */
