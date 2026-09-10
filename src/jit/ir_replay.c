/* ir_replay.c — working out what the interpreter does across a hand-over.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/debug.h"
#include "cscript/memory.h"
#include "cscript/opcode.h"
#include "cscript/shape.h"
#include "cscript/type.h"
#include "cscript/vm.h"

#include "jit/ir_internal.h"

/* The IR's view of what a constant holds. */
IrType csIrTypeOfConstant(Value constant) {
  return IS_NUMBER(constant) ? IR_TYPE_NUMBER
         : IS_BOOL(constant) ? IR_TYPE_BOOL
                             : IR_TYPE_UNKNOWN;
}

/* ---- replaying a hand-over ----------------------------------------------
 *
 * A hand-over used to end the lowering for good. Everything after an exit runs
 * in the interpreter, so the operand-stack height there was unknown — and in
 * this VM a local *is* a stack slot, so a block whose entry height is unknown
 * cannot be lowered at all: the wrong height means the wrong slot, silently.
 *
 * That cost more than it sounds. A script that declares a function before its
 * hot loop hands the frame back on the declaration, at offset zero, and lost
 * the loop with it — which is every script of the shape
 *
 *     function f(x) { ... }
 *     for (let i = 0; i < n; i++) total = total + f(i);
 *
 * But the height is only unknown because nothing worked it out. Where every
 * instruction in the skipped run is one whose effect on the frame is fixed,
 * both the height and what each slot ends up holding follow from the bytecode
 * — and it is the same bytecode the interpreter is about to run, so the two
 * cannot disagree. It is a proof rather than a guess, which is what separates
 * this from speculating on the height and checking it later.
 *
 * Anything the model does not cover still gives up, exactly as before.
 */

static bool replayPush(IrType *slotType, int *height, IrType type) {
  if (*height < 0 || *height >= IR_MAX_STACK) return false;
  slotType[*height] = type;
  (*height)++;
  return true;
}

static bool replayPop(int *height, int count) {
  if (count < 0 || *height < count) return false;
  *height -= count;
  return true;
}

/* A jump can only be the *last* instruction of a run being replayed: the scan
 * that finds where the run ends stops at the first leader, and a jump makes the
 * instruction after it one. So there is at most one of them, and modelling the
 * fall-through is only half the answer — the other arm is an incoming path that
 * the block it lands on has to know about, or every entry type derived for that
 * block comes from the wrong set of paths. ReplayJump, in ir_internal.h, is
 * what carries it back to the walk.
 */

/* How many values a jump takes off the stack before it goes, and whether it
 * may also fall through. Returns false for a jump this does not model. */
static bool jumpEffect(uint8_t opcode, int *pops, bool *fallsThrough) {
  *fallsThrough = true;
  switch (opcode) {
    /* Unconditional, so the instruction after it is not reached from here. */
    case OP_JUMP:
    case OP_LOOP:
      *pops = 0;
      *fallsThrough = false;
      return true;
    /* These test the value and leave it, because the expression they are part
     * of still wants it — `a && b` yields an operand, not a boolean. */
    case OP_JUMP_IF_FALSE:
    case OP_JUMP_IF_TRUE:
      *pops = 0;
      return true;
    case OP_POP_JUMP_IF_FALSE:
      *pops = 1;
      return true;
    /* The fused compare-and-branch forms consume both operands on either arm,
     * so the two arrive at the same depth. */
    case OP_JUMP_IF_NOT_LESS:
    case OP_JUMP_IF_NOT_LESS_EQUAL:
    case OP_JUMP_IF_NOT_GREATER:
    case OP_JUMP_IF_NOT_GREATER_EQUAL:
    case OP_JUMP_IF_EQUAL:
    case OP_JUMP_IF_NOT_EQUAL:
      *pops = 2;
      return true;
    default:
      return false;
  }
}

/* Returns the operand-stack height at `to`, or -1 when the run holds something
 * this does not model. `slotType` is written only on success, so a run given
 * up on halfway leaves the lowering's view of the frame untouched. `jump`
 * receives the taken arm of the run's final jump, if it has one, and `refusal`
 * the opcode it could not model, for the tiering report. */
int csIrReplayHandedOver(const Chunk *chunk, IrType *slotType, int from, int to,
                            int height, ReplayJump *jump, const char **refusal) {
  IrType replayed[IR_MAX_STACK];
  memcpy(replayed, slotType, sizeof replayed);

  jump->target = -1;
  jump->fallsThrough = true;
  *refusal = NULL;

  for (int offset = from; offset < to;) {
    uint8_t opcode = chunk->code[offset];
    bool ok = true;

    switch (opcode) {
      /* Values made out of nothing. */
      case OP_CONSTANT: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        if (index < 0 || index >= chunk->constants.count) return -1;
        ok = replayPush(replayed, &height, csIrTypeOfConstant(chunk->constants.values[index]));
        break;
      }
      case OP_TRUE:
      case OP_FALSE:
        ok = replayPush(replayed, &height, IR_TYPE_BOOL);
        break;
      case OP_NULL:
      case OP_UNDEFINED:
      /* A closure, and a global whose value at this moment says nothing about
       * its value when the interpreter gets here. */
      case OP_CLOSURE:
      case OP_GET_GLOBAL:
        ok = replayPush(replayed, &height, IR_TYPE_UNKNOWN);
        break;

      /* Values moved from somewhere the model already knows about. */
      case OP_GET_LOCAL:
      case OP_GET_LOCAL_LOCAL:
      case OP_GET_LOCAL_CONST: {
        int wanted = opcode == OP_GET_LOCAL_LOCAL ? 2 : 1;
        for (int which = 1; which <= wanted && ok; which++) {
          int slot = chunk->code[offset + which];
          if (slot < 0 || slot >= IR_MAX_STACK) return -1;
          ok = replayPush(replayed, &height, replayed[slot]);
        }
        if (ok && opcode == OP_GET_LOCAL_CONST) {
          int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
          if (index < 0 || index >= chunk->constants.count) return -1;
          ok = replayPush(replayed, &height,
                          csIrTypeOfConstant(chunk->constants.values[index]));
        }
        break;
      }
      case OP_DUP:
        if (height < 1) return -1;
        ok = replayPush(replayed, &height, replayed[height - 1]);
        break;

      /* Values discarded. */
      case OP_POP:
      case OP_DEFINE_GLOBAL:
      case OP_DEFINE_CONST:
      case OP_SET_GLOBAL_POP:
        ok = replayPop(&height, 1);
        break;
      case OP_POP_N:
        ok = replayPop(&height, chunk->code[offset + 1]);
        break;
      /* Leaves what it stored, so the height is unchanged. */
      case OP_SET_GLOBAL:
        if (height < 1) return -1;
        break;

      /* Values put into a slot, which is the part that matters as much as the
       * height: a loop counter is established by one of these. */
      case OP_SET_LOCAL:
      case OP_SET_LOCAL_POP: {
        int slot = chunk->code[offset + 1];
        if (slot < 0 || slot >= IR_MAX_STACK || height < 1) return -1;
        replayed[slot] = replayed[height - 1];
        if (opcode == OP_SET_LOCAL_POP) ok = replayPop(&height, 1);
        break;
      }
      /* Both refuse a slot that is not a number, so reaching the next
       * instruction at all proves one is there. */
      case OP_INC_LOCAL:
      case OP_DEC_LOCAL: {
        int slot = chunk->code[offset + 1];
        if (slot < 0 || slot >= IR_MAX_STACK) return -1;
        replayed[slot] = IR_TYPE_NUMBER;
        break;
      }

      /* Arithmetic, which throws rather than coerce — so a result exists only
       * where it is the type the operator produces. `+` is the exception: it
       * concatenates, so nothing is known about what it leaves. */
      case OP_ADD_NUM:
      case OP_SUBTRACT:
      case OP_MULTIPLY:
      case OP_DIVIDE:
      case OP_MODULO:
        ok = replayPop(&height, 2) && replayPush(replayed, &height, IR_TYPE_NUMBER);
        break;
      case OP_NEGATE:
        ok = replayPop(&height, 1) && replayPush(replayed, &height, IR_TYPE_NUMBER);
        break;
      case OP_ADD:
        ok = replayPop(&height, 2) && replayPush(replayed, &height, IR_TYPE_UNKNOWN);
        break;
      case OP_LESS: case OP_LESS_EQUAL: case OP_GREATER: case OP_GREATER_EQUAL:
      case OP_EQUAL: case OP_NOT_EQUAL:
        ok = replayPop(&height, 2) && replayPush(replayed, &height, IR_TYPE_BOOL);
        break;
      case OP_NOT:
        ok = replayPop(&height, 1) && replayPush(replayed, &height, IR_TYPE_BOOL);
        break;

      /* A completed call leaves one value where the callee and its arguments
       * were, whatever it did in between. What it did in between is the
       * interpreter's business: every assumption the compiled code holds —
       * the global table's version, the shapes, the inlined callees — is
       * checked on the way in, which is after all of this has run. */
      case OP_CALL:
        ok = replayPop(&height, chunk->code[offset + 1] + 1) &&
             replayPush(replayed, &height, IR_TYPE_UNKNOWN);
        break;
      case OP_INVOKE:
        ok = replayPop(&height, chunk->code[offset + 3] + 1) &&
             replayPush(replayed, &height, IR_TYPE_UNKNOWN);
        break;

      /* A return, after which nothing flows anywhere: the interpreter leaves
       * the function, so the end of the run is not reached along this path and
       * the height there is not a fact about anything. Whatever follows is
       * dead code, so there is no point modelling it either. */
      case OP_RETURN:
        jump->target = -1;
        jump->fallsThrough = false;
        memcpy(slotType, replayed, sizeof replayed);
        return height;

      /* A jump, which can only be the last instruction of the run — see the
       * note on ReplayJump. Its taken arm is recorded for the caller to merge
       * into the block it lands on; what is modelled here is the other arm. */
      case OP_JUMP:
      case OP_LOOP:
      case OP_JUMP_IF_FALSE:
      case OP_JUMP_IF_TRUE:
      case OP_POP_JUMP_IF_FALSE:
      case OP_JUMP_IF_NOT_LESS:
      case OP_JUMP_IF_NOT_LESS_EQUAL:
      case OP_JUMP_IF_NOT_GREATER:
      case OP_JUMP_IF_NOT_GREATER_EQUAL:
      case OP_JUMP_IF_EQUAL:
      case OP_JUMP_IF_NOT_EQUAL: {
        int pops = 0;
        bool falls = true;
        if (!jumpEffect(opcode, &pops, &falls)) return -1;
        if (!replayPop(&height, pops)) return -1;

        int past = csInstructionLength(chunk, offset);
        if (past <= offset) return -1;
        /* If it is not the last instruction of the run, the assumption that
         * put it there is wrong and nothing below is safe to believe. */
        if (past != to) return -1;

        int reach = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        jump->target = opcode == OP_LOOP ? past - reach : past + reach;
        jump->height = height;
        jump->fallsThrough = falls;
        memcpy(jump->slotType, replayed, sizeof jump->slotType);
        break;
      }

      default:
        *refusal = csOpcodeName((OpCode)opcode);
        return -1;
    }
    if (!ok) {
      *refusal = csOpcodeName((OpCode)opcode);
      return -1;
    }

    int next = csInstructionLength(chunk, offset);
    if (next <= offset) return -1;
    offset = next;
  }

  memcpy(slotType, replayed, sizeof replayed);
  return height;
}
