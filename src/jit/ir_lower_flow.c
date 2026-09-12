/* ir_lower_flow.c — jumps, branches, calls and returns —
 * one of the four groups the lowering's instruction walk asks in
 * turn. Each keeps the switch its cases were written in and answers
 * LOWER_UNHANDLED for an opcode belonging to another, so what happens to an
 * opcode none of them claims is the hand-over the lowering has always done.
 * See ir.c for the walk and ir_internal.h for what they share.
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

LowerResult csIrLowerFlow(LowerAt *at) {
  Lowering *low = at->low;
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  const ObjFunction *function = at->function;
  const int next = at->next;
  const int line = at->line;
  const int jumpTarget = at->jumpTarget;
  const Chunk *chunk = at->chunk;
  const int offset = at->offset;
  const uint8_t opcode = chunk->code[offset];

  switch (opcode) {
    /* A fused compare-and-branch is two IR instructions: the comparison and
     * the branch on it. Splitting them is what lets a backend keep the
     * comparison in flags. */
    case OP_JUMP_IF_NOT_LESS:
    case OP_JUMP_IF_NOT_LESS_EQUAL:
    case OP_JUMP_IF_NOT_GREATER:
    case OP_JUMP_IF_NOT_GREATER_EQUAL:
    case OP_JUMP_IF_NOT_EQUAL:
    case OP_JUMP_IF_EQUAL: {
      IrOp comparison = opcode == OP_JUMP_IF_NOT_LESS          ? IR_LT
                        : opcode == OP_JUMP_IF_NOT_LESS_EQUAL  ? IR_LE
                        : opcode == OP_JUMP_IF_NOT_GREATER     ? IR_GT
                        : opcode == OP_JUMP_IF_NOT_GREATER_EQUAL ? IR_GE
                        : opcode == OP_JUMP_IF_EQUAL           ? IR_NE
                                                               : IR_EQ;
      if (!csIrLowerBinary(low, block, comparison, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      int condition = csIrPop(low, block, line);

      IrInst *branch = csIrAppend(block, IR_BRANCH, line);
      branch->a = condition;
      /* These jump when the comparison is *false*, which is why the arms
       * read the way they do: taken means fall through. */
      branch->b = csIrBlockAt(ir, next);
      branch->c = csIrBlockAt(ir, jumpTarget);
      if (!csIrReachBlock(low, branch->b, low->stackTop)) return LOWER_FAILED;
      if (!csIrReachBlock(low, branch->c, low->stackTop)) return LOWER_FAILED;
      break;
    }

    case OP_LOOP: {
      int target = next - ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
      IrInst *jump = csIrAppend(block, IR_JUMP, line);
      jump->a = csIrBlockAt(ir, target);
      if (!csIrReachBlock(low, jump->a, low->stackTop)) return LOWER_FAILED;
      break;
    }

    case OP_JUMP: {
      IrInst *jump = csIrAppend(block, IR_JUMP, line);
      jump->a = csIrBlockAt(ir, jumpTarget);
      if (!csIrReachBlock(low, jump->a, low->stackTop)) return LOWER_FAILED;
      break;
    }

    case OP_POP_JUMP_IF_FALSE:
    case OP_JUMP_IF_FALSE:
    case OP_JUMP_IF_TRUE: {
      /* These two leave the value on the stack, so it is read rather than
       * popped — and *loaded from its slot* rather than taken out of the
       * lowering's abstract stack.
       *
       * That distinction is the whole of a miscompile this once produced.
       * `a || b || c` compiles to a chain of these, and the jump at the end of
       * each arm lands on the next one — so this instruction begins a block
       * with two predecessors, and the register the linear walk remembers was
       * produced on only one of them. Branching on it meant branching on
       * whatever that register happened to hold, and `percentEncode(".")`
       * escaped a character it should have left alone. The slot is the one
       * thing both paths agree on. */
      int condition;
      if (opcode == OP_POP_JUMP_IF_FALSE) {
        condition = csIrPop(low, block, line);
      } else {
        if (low->stackTop == 0) {
          low->reason = "operand stack underflow while lowering";
          return LOWER_FAILED;
        }
        IrType held = low->slotType[low->stackTop - 1];
        condition = csIrNewRegister(ir, held);
        IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
        load->type = held;
        load->result = condition;
        load->a = low->stackTop - 1;
      }
      if (low->reason != NULL) return LOWER_FAILED;

      IrInst *branch = csIrAppend(block, IR_BRANCH, line);
      branch->a = condition;
      if (opcode == OP_JUMP_IF_TRUE) {
        branch->b = csIrBlockAt(ir, jumpTarget);
        branch->c = csIrBlockAt(ir, next);
      } else {
        branch->b = csIrBlockAt(ir, next);
        branch->c = csIrBlockAt(ir, jumpTarget);
      }
      if (!csIrReachBlock(low, branch->b, low->stackTop)) return LOWER_FAILED;
      if (!csIrReachBlock(low, branch->c, low->stackTop)) return LOWER_FAILED;
      break;
    }

    /* A call whose callee this lowering put a placeholder there for, which
     * is the only kind it ever sees: the body replaces the call, and the
     * frame the VM would have built is never opened.
     *
     * The arguments are loaded out of their frame positions rather than
     * taken from the abstract stack, because a block can be entered by a
     * jump and the register that pushed a value on one path may not have
     * run on another. The frame slot holds it either way. */
    case OP_CALL: {
      int argCount = chunk->code[offset + 1];
      int base = low->stackTop - argCount - 1;
      if (base < 0 || base >= IR_MAX_STACK || low->pendingCallee[base] == NULL) {
        return LOWER_HAND_OVER;
      }
      if (ir->inlinedInstructions >= IR_INLINE_MAX_TOTAL) return LOWER_HAND_OVER;

      ObjClosure *closure = low->pendingCallee[base];
      int emittedBefore = block->count;
      int heightBefore = low->stackTop;

      int args[IR_MAX_STACK];
      bool loaded = true;
      for (int a = argCount - 1; a >= 0 && loaded; a--) {
        args[a] = csIrPop(low, block, line);
        if (low->reason != NULL) {
          low->reason = NULL;
          loaded = false;
        }
      }

      int result = loaded ? csIrInlineCallee(ir, block, closure->function, args,
                                         argCount, line)
                          : -1;
      if (result >= 0 &&
          !csIrRememberInlinedCall(ir, &function->module->globals,
                               AS_STRING(chunk->constants.values[low->pendingName[base]]),
                               closure)) {
        result = -1;
      }
      if (result < 0) {
        /* Nothing of the attempt survives: the emitted instructions go, the
         * abstract stack goes back to where the call found it, and the frame
         * is handed over at the floor exactly as an unlowerable call always
         * was. */
        block->count = emittedBefore;
        low->stackTop = heightBefore;
        return LOWER_HAND_OVER;
      }

      low->stackTop = base; /* the placeholder held no value to load */
      low->pendingCallee[base] = NULL;
      low->pendingCount--;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    case OP_RETURN: {
      int value = csIrPop(low, block, line);
      if (low->reason != NULL) return LOWER_FAILED;
      IrInst *inst = csIrAppend(block, IR_RETURN, line);
      inst->a = value;
      break;
    }

    default:
      return LOWER_UNHANDLED;
  }
  return LOWER_OK;
}
