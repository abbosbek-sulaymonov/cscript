/* jitcode_alloc.c — deciding where every value lives.
 *
 * Without this the compiled code was *slower* than the interpreter: every IR
 * value went to memory and came back, so a two-multiply function did more
 * loads than the bytecode did, and removing dispatch bought less than that
 * cost. A linear scan per block is enough, because the IR is nearly all
 * block-local once the redundant slot round-trips have been forwarded.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cscript/jitcode.h"
#include "cscript/object.h"

#include "jit/jitcode_internal.h"

#if defined(__APPLE__) && defined(__arm64__) && CS_NAN_BOXING
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#define CS_JIT_APPLE_SILICON 1
#endif

/* Which arm64 condition code each comparison wants. The *ordered* forms: an
 * unordered compare — either side NaN — must not take the branch, which is
 * what distinguishes MI/GT/LS from LT/GE and matters for `x < NaN`. */
uint32_t csJitConditionFor(IrOp op) {
  switch (op) {
    case IR_LT: return COND_MI;
    case IR_LE: return COND_LS;
    case IR_GT: return COND_GT;
    case IR_GE: return COND_GE;
    case IR_EQ: return COND_EQ;
    default: return COND_NE;
  }
}

/* ---- register allocation ------------------------------------------------
 *
 * Without this the compiled code was *slower* than the interpreter: every IR
 * value went to memory and came back, so a two-multiply function did more
 * loads than the bytecode did. Removing dispatch bought less than that cost.
 *
 * A linear scan per block, which is enough because the IR is nearly all
 * block-local once redundant slot round-trips have been forwarded. A value
 * used outside the block that defined it keeps a memory home — proving when
 * that is safe needs liveness across the whole graph, and the values it would
 * catch are rare.
 *
 * d0 and d1 stay free as scratch for operands that did not get a register.
 * d8–d15 are avoided because they are callee-saved and using them would mean
 * a prologue; d2–d7 and d16–d31 are free for the taking, which is 22 and more
 * than any block here needs.
 */

/* A function that calls out allocates only from d8–d15.
 *
 * Those are the registers a call is obliged to preserve, so nothing live has
 * to be spilled around one — which is the whole reason calling out is cheap
 * enough to be worth doing. It costs fourteen registers, and a loop that fits
 * in eight is most of them. */

int csJitPoolSize(bool callSafe) {
  return callSafe ? ALLOC_CALLSAFE_POOL_SIZE : ALLOC_POOL_SIZE;
}

int csJitAllocPool(int index, bool callSafe) {
  if (callSafe) return 8 + index;
  /* d2..d7, then d16..d31 — skipping the callee-saved bank entirely. */
  return index < 6 ? 2 + index : 16 + (index - 6);
}

/* Which frame slots can live in a register for the whole function.
 *
 * This is where a loop stops touching memory. A counter and an accumulator are
 * frame locals, so without it every iteration loads and stores them exactly as
 * the interpreter does — which is why register allocation alone left the loop
 * benchmark unchanged.
 *
 * A slot qualifies when every value ever stored into it is a proved number.
 * Anything else keeps its memory home: a boolean is a NaN-boxed singleton, and
 * while moving one through a floating-point register happens to preserve its
 * bits, deriving that from the hardware manual is a worse foundation than
 * simply not doing it.
 *
 * Nothing else can observe these slots. The array belongs to the call, the
 * compiled code makes no calls of its own, and the IR refuses any function
 * that captures a local — so there is nothing to write back. */
bool *csJitPromotableSlots(const IrFunction *ir) {
  bool *promotable = (bool *)malloc(sizeof(bool) * (size_t)(ir->slotCount + 1));
  for (int s = 0; s <= ir->slotCount; s++) promotable[s] = true;

  /* Slot 0 is the callee and the arguments follow it: those arrive as Values
   * the caller wrote, and only a parameter the checker typed is known. */
  promotable[0] = false;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op != IR_STORE_LOCAL) continue;
      if (inst->a < 0 || inst->a > ir->slotCount) continue;
      if (inst->b < 0 || inst->b >= ir->registerCount || ir->registerTypes[inst->b] != IR_TYPE_NUMBER) {
        promotable[inst->a] = false;
      }
    }
  }

  /* A parameter is only known to be a number if it was declared one. */
  for (int s = 1; s <= ir->slotCount; s++) {
    if (ir->slotTypes != NULL && s <= ir->slotCount && ir->slotTypes[s] != IR_TYPE_NUMBER) {
      /* Still promotable if nothing outside the function put a value there —
       * that is, if it is a temporary rather than an argument. */
      if (s <= ir->source->arity) promotable[s] = false;
    }
  }
  return promotable;
}

/* -1 means the value lives in the scratch array rather than a register. */
int *csJitAllocateRegisters(const IrFunction *ir, int reserved, bool callSafe) {
  int *assignment = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  int *definedIn = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  bool *escapes = (bool *)calloc((size_t)ir->registerCount + 1, sizeof(bool));

  for (int r = 0; r <= ir->registerCount; r++) {
    assignment[r] = -1;
    definedIn[r] = -1;
  }

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->result >= 0) definedIn[inst->result] = b;
    }
  }

  /* A value read in a block other than the one that defined it — or read
   * before it, which a loop back-edge makes possible — stays in memory. */
  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      int operands[2] = {inst->a, inst->b};
      /* For these, `a` is a slot number rather than a value. */
      /* `a` is not a value for these: a slot, a block, or — for an exit — a
       * bytecode offset and a stack height. */
      if (inst->op == IR_LOAD_LOCAL || inst->op == IR_JUMP) operands[0] = -1;
      if (inst->op == IR_STORE_LOCAL || inst->op == IR_EXIT) operands[0] = -1;
      if (inst->op == IR_LOAD_GLOBAL || inst->op == IR_STORE_GLOBAL) operands[0] = -1;
      if (inst->op == IR_EXIT || inst->op == IR_LOAD_GLOBAL) operands[1] = -1;
      if (inst->op == IR_BRANCH || inst->op == IR_RETURN || inst->op == IR_NEG || inst->op == IR_CONST || inst->op == IR_LOAD_LOCAL) {
        operands[1] = -1;
      }
      for (int k = 0; k < 2; k++) {
        int value = operands[k];
        if (value < 0 || value >= ir->registerCount) continue;
        if (definedIn[value] != b) escapes[value] = true;
      }
    }
  }

  /* Last use within the defining block, so a register can be handed back. */
  int *lastUse = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  for (int r = 0; r <= ir->registerCount; r++) lastUse[r] = -1;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int r = 0; r <= ir->registerCount; r++) lastUse[r] = -1;
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->a >= 0 && inst->a < ir->registerCount && inst->op != IR_LOAD_LOCAL && inst->op != IR_STORE_LOCAL && inst->op != IR_JUMP) {
        lastUse[inst->a] = i;
      }
      if (inst->op == IR_STORE_LOCAL && inst->b >= 0 && inst->b < ir->registerCount) {
        lastUse[inst->b] = i;
      }
      if (inst->b >= 0 && inst->b < ir->registerCount && inst->op != IR_STORE_LOCAL && inst->op != IR_BRANCH && inst->op != IR_RETURN && inst->op != IR_NEG &&
          inst->op != IR_CONST && inst->op != IR_LOAD_LOCAL && inst->op != IR_JUMP) {
        lastUse[inst->b] = i;
      }
    }

    int pool = csJitPoolSize(callSafe);
    bool taken[ALLOC_POOL_SIZE];
    int holder[ALLOC_POOL_SIZE];
    for (int k = 0; k < pool; k++) {
      taken[k] = false;
      holder[k] = -1;
    }

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      /* Free anything whose last use was the previous instruction. */
      for (int k = 0; k < pool; k++) {
        if (taken[k] && holder[k] >= 0 && lastUse[holder[k]] >= 0 && lastUse[holder[k]] < i) {
          taken[k] = false;
          holder[k] = -1;
        }
      }

      if (inst->result < 0 || escapes[inst->result]) continue;
      /* Comparison results live in memory; see the encoder. */
      if (inst->op == IR_LT || inst->op == IR_LE || inst->op == IR_GT || inst->op == IR_GE || inst->op == IR_EQ || inst->op == IR_NE) {
        continue;
      }
      if (lastUse[inst->result] < 0) continue; /* never read: no register needed */

      for (int k = 0; k < pool - reserved; k++) {
        if (taken[k]) continue;
        taken[k] = true;
        holder[k] = inst->result;
        assignment[inst->result] = csJitAllocPool(k, callSafe);
        break;
      }
    }
  }

  free(definedIn);
  free(escapes);
  free(lastUse);
  return assignment;
}

/* The register holding a value: its own, or a scratch one it is loaded into. */
int csJitReadOperand(Encoder *encoder, const int *home, int value, int scratch) {
  if (home[value] >= 0) return home[value];
  csJitLdrDouble(encoder, scratch, REG_SCRATCH, value * 8);
  return scratch;
}

/* Whether every block can be entered with nothing live in a register.
 *
 * Registers in this IR are operand-stack positions, so a register live across
 * a block boundary means the operand stack was not empty there. Where no block
 * reads a register it did not itself write, all of a function's live state is
 * in its slots — and the interpreter and the compiled code agree, byte for
 * byte, on where those are. That agreement is what makes on-stack replacement
 * a jump rather than a translation, so it is checked rather than assumed. */
bool csJitBlocksAreSelfContained(const IrFunction *ir) {
  bool *written = (bool *)calloc((size_t)ir->registerCount + 1, sizeof(bool));
  bool clean = true;

  for (int b = 0; b < ir->blockCount && clean; b++) {
    memset(written, 0, sizeof(bool) * ((size_t)ir->registerCount + 1));

    for (int i = 0; i < ir->blocks[b].count && clean; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      int reads[2];
      csIrRegisterOperands(inst, &reads[0], &reads[1]);

      for (int r = 0; r < 2; r++) {
        if (reads[r] >= 0 && reads[r] <= ir->registerCount && !written[reads[r]]) {
          clean = false;
        }
      }
      if (inst->result >= 0 && inst->result <= ir->registerCount) {
        written[inst->result] = true;
      }
    }
  }

  free(written);
  return clean;
}

/* A block reached by an edge that does not move forward is a loop header. */
bool csJitIsLoopHeader(const IrFunction *ir, int target) {
  for (int b = target; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op == IR_JUMP && inst->a == target) return true;
      if (inst->op == IR_BRANCH && (inst->b == target || inst->c == target)) return true;
    }
  }
  return false;
}
