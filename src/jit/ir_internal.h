/* ir_internal.h — what the lowering's files share, and nothing outside it.
 *
 * The public shape of the IR is in include/cscript/ir.h. This is the seam
 * between the pieces that produce it: the abstract-interpretation state, the
 * three-way answer an instruction's lowering gives, and the handful of
 * operations every one of them is built out of.
 *
 * Split the way the parser and the compiler were, by what each file handles
 * rather than by phase — the opcode cases are mutually independent but all
 * need the same state, so layering them would have been a fiction.
 */
#ifndef CSCRIPT_JIT_IR_INTERNAL_H
#define CSCRIPT_JIT_IR_INTERNAL_H

#include "cscript/chunk.h"
#include "cscript/common.h"
#include "cscript/ir.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/table.h"

/* The operand stack and the locals are the same array in this VM, so the model
 * of one bounds both; IR_MAX_SLOTS is where that number is explained. */
#define IR_MAX_STACK IR_MAX_SLOTS
#define IR_MAX_BLOCKS 256

/* How much body is worth splicing, per call and per caller. Both bounds are
 * arbitrary and both are the point: inlining trades code size for calls
 * removed, and without a limit a chain of small functions is one large one. */
#define IR_INLINE_MAX_OPS 32
#define IR_INLINE_MAX_TOTAL 128

/* ---- the abstract interpretation ---------------------------------------- */

typedef struct {
  IrFunction *ir;
  const Chunk *chunk;
  int stack[IR_MAX_STACK]; /* virtual register per operand-stack slot */
  int stackTop;
  int blockOf[IR_MAX_BLOCKS]; /* bytecode offset -> block index */

  /* The operand-stack height each block is entered with.
   *
   * Linear order is not enough to know it: a block reached by a jump inherits
   * the height at the *jump*, which may differ from the height the preceding
   * instruction happened to leave. Recording it when the jump is emitted, and
   * adopting it on arrival, is what keeps the model and the real frame in
   * agreement — and a disagreement between two predecessors is refused rather
   * than guessed at. */
  int entryHeight[IR_MAX_BLOCKS];

  /* What is known about each frame slot. Seeded from the declared parameter
   * types and updated as stores go by; a slot written with two different types
   * falls back to unknown and stays there, which is the conservative answer
   * and costs only a missed specialisation. */
  IrType slotType[IR_MAX_STACK];

  /* A callable pushed for a call whose body is about to replace it.
   *
   * The position holds no register at all — see OP_GET_GLOBAL in
   * ir_lower_data.c — so anything that read it as a value would be reading
   * nothing. What keeps that from happening is that the placeholder is only
   * ever pushed once the call that takes it off has been found, in the same
   * straight run of instructions and with the right number of arguments
   * between them. */
  ObjClosure *pendingCallee[IR_MAX_STACK];
  int pendingName[IR_MAX_STACK]; /* the binding it was read from */
  int pendingCount;

  /* What layout each frame slot's object has *here*, for the slots a store
   * adds to. An add transitions the object, so the next store on the same slot
   * expects what the last one produced rather than what it had at entry — the
   * one fact that makes a constructor's chain of `this.x = x` lowerable.
   *
   * NULL for a slot nothing has added to, which is nearly all of them. Reset
   * per block, because only a straight run of instructions proves the chain
   * ran in that order. */
  Shape *slotShape[IR_MAX_SLOTS];

  const char *reason;
} Lowering;

/* ir_allocate.c — building an object from compiled code, and the slots that
 * involves. */
int csIrAddLiteral(IrFunction *ir, ObjString **keys, int count);
int csIrAddCall(IrFunction *ir, Table *globals, ObjString *name, ObjClosure *callee, int argCount);

/* ---- lowering one instruction ------------------------------------------- */

/* What lowering an instruction concluded.
 *
 * Three answers rather than two, and the third is what makes the split
 * possible: the cases used to say `goto handOver` and `goto failed`, which a
 * function cannot do to its caller. LOWER_UNHANDLED is the fourth, and is not
 * an outcome at all — it means "not mine", so the walk asks the next group. */
typedef enum {
  LOWER_OK,        /* emitted; carry on with the next instruction */
  LOWER_HAND_OVER, /* the interpreter takes the frame from here */
  LOWER_FAILED,    /* the whole function is refused; low->reason says why */
  LOWER_UNHANDLED, /* another group's opcode */
} LowerResult;

/* Everything about the instruction being lowered, so a case needs one
 * argument rather than nine. */
typedef struct {
  Lowering *low;
  IrFunction *ir;
  IrBlock *block;
  const Chunk *chunk;
  const ObjFunction *function;
  const bool *leader;
  int offset; /* of this instruction */
  int next;   /* of the one after it */
  int line;
  int jumpTarget; /* where a jump here would land; meaningless otherwise */
} LowerAt;

/* ir_allocate.c, again: a call the splice would not take. Declared here rather
 * than above because it needs LowerAt. */
int csIrLowerRealCall(LowerAt *at, ObjClosure *closure, int base, const int *args, int argCount);

LowerResult csIrLowerData(LowerAt *at);
LowerResult csIrLowerObject(LowerAt *at);
LowerResult csIrLowerArith(LowerAt *at);
LowerResult csIrLowerFlow(LowerAt *at);

/* ---- building ----------------------------------------------------------- */

int csIrNewRegister(IrFunction *ir, IrType type);
IrInst *csIrAppend(IrBlock *block, IrOp op, int line);

/* Every jump target starts a block, and so does the instruction after a jump. */
bool csIrMarkLeaders(const Chunk *chunk, bool *leader, const char **reason);

/* Pushing stores the value at its stack position and popping loads it back,
 * because the operand stack and the locals are the same array. */
bool csIrPush(Lowering *low, IrBlock *block, int reg, int line);
int csIrPop(Lowering *low, IrBlock *block, int line);

int csIrBlockAt(const IrFunction *ir, int offset);
bool csIrLowerBinary(Lowering *low, IrBlock *block, IrOp op, IrType type, int line);
void csIrClearPendingCallees(Lowering *low);

/* Notes that a block is entered with `height` on the stack, and with the slots
 * holding what they hold at the jump. */
bool csIrReachBlock(Lowering *low, int target, int height);

/* The same, for a path the walk did not take: the taken arm of a jump inside a
 * run handed over to the interpreter. False when the heights disagree. */
bool csIrRecordArrival(IrFunction *ir, Lowering *low, int block, const IrType *slotType, int height);

/* What the module holds under that name, asked at the moment the function
 * turns hot — which is the only moment the lowering has a running program to
 * ask. What keeps the answer true afterwards is checked again at every entry. */
bool csIrGlobalHoldsNumber(const ObjFunction *function, const Chunk *chunk, int nameIndex);
ObjClosure *csIrGlobalCallable(const ObjFunction *function, const Chunk *chunk, int nameIndex);

/* The IR's view of what a constant holds. */
IrType csIrTypeOfConstant(Value constant);

/* ---- inlining ----------------------------------------------------------- */

/* Whether this function's body can go where a call to it is. */
bool csIrCalleeIsInlinable(const ObjFunction *callee, int argCount);
bool csIrCalleeIsCallable(const ObjFunction *callee, int argCount);

/* Where the call a callee load feeds is, or -1. */
int csIrCallSiteFor(const Chunk *chunk, const bool *leader, int calleeOffset, int *argCountOut);

/* Splices a callee's body into the block the call was being lowered into, and
 * answers the register holding the result — or -1, having emitted instructions
 * the caller must throw away. */
int csIrInlineCallee(IrFunction *ir, IrBlock *block, const ObjFunction *callee, const int *args, int argCount, int line);

/* Records the binding an inlined callee was read from, once per callee. */
bool csIrRememberInlinedCall(IrFunction *ir, Table *globals, ObjString *name, ObjClosure *callee);

/* ---- replaying a hand-over ---------------------------------------------- */

/* The taken arm of a jump inside a run being replayed. `fallsThrough` is false
 * for an unconditional jump or a return, where the run's end is not reached at
 * all and the height there means nothing. */
typedef struct {
  int target; /* bytecode offset of the taken arm, or -1 */
  int height;
  IrType slotType[IR_MAX_STACK];
  bool fallsThrough;
} ReplayJump;

/* The operand-stack height at `to`, or -1 when the run holds something this
 * does not model. `slotType` is written only on success. */
int csIrReplayHandedOver(const Chunk *chunk, IrType *slotType, int from, int to, int height, ReplayJump *jump, const char **refusal);

#endif /* CSCRIPT_JIT_IR_INTERNAL_H */
