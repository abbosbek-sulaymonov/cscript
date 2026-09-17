/* ir_lower_data.c — constants, globals, locals and the operand stack —
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

LowerResult csIrLowerData(LowerAt *at) {
  Lowering *low = at->low;
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  const ObjFunction *function = at->function;
  const bool *leader = at->leader;
  const int line = at->line;
  const Chunk *chunk = at->chunk;
  const int offset = at->offset;
  const uint8_t opcode = chunk->code[offset];

  switch (opcode) {
    case OP_CONSTANT: {
      int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
      Value constant = chunk->constants.values[index];
      IrType type = IS_NUMBER(constant) ? IR_TYPE_NUMBER : IS_BOOL(constant) ? IR_TYPE_BOOL : IR_TYPE_UNKNOWN;
      int result = csIrNewRegister(ir, type);
      IrInst *inst = csIrAppend(block, IR_CONST, line);
      inst->result = result;
      inst->constant = constant;
      inst->type = type;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    /* Globals. Only where the module already holds a number under that name:
     * the checker will not let a declared binding change type, and nothing a
     * compiled region can do calls anything, so the only writer is this code
     * and every store it makes is proved numeric. Anything else hands the
     * frame back. */
    case OP_GET_GLOBAL: {
      int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
      if (!csIrGlobalHoldsNumber(function, chunk, index)) {
        /* Not a number, but perhaps a small function about to be called —
         * in which case its body goes where the call is and no value for
         * the callee is ever needed.
         *
         * The call is found before anything is pushed. A position holding
         * no register has to be taken off by the instruction it was pushed
         * for, so proving that instruction exists is what makes pushing it
         * safe; without the proof this falls through to the hand-over the
         * lowering has always done here. */
        int argCount = 0;
        int callAt = csIrCallSiteFor(chunk, leader, offset, &argCount);
        ObjClosure *closure = callAt < 0 ? NULL : csIrGlobalCallable(function, chunk, index);
        /* Inlinable, or at least callable: a body the splice will not take is
         * what the call exists for, and the placeholder is the same either
         * way — what differs is what the OP_CALL below does with it. */
        if (closure == NULL || (!csIrCalleeIsInlinable(closure->function, argCount) && !csIrCalleeIsCallable(closure->function, argCount))) {
          return LOWER_HAND_OVER;
        }
        if (low->stackTop >= IR_MAX_STACK) {
          low->reason = "expression stack too deep";
          return LOWER_FAILED;
        }

        low->stack[low->stackTop] = -1;
        low->slotType[low->stackTop] = IR_TYPE_UNKNOWN;
        low->pendingCallee[low->stackTop] = closure;
        low->pendingName[low->stackTop] = index;
        low->pendingCount++;
        low->stackTop++;
        if (low->stackTop > ir->slotCount) ir->slotCount = low->stackTop;
        break;
      }

      int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *inst = csIrAppend(block, IR_LOAD_GLOBAL, line);
      inst->result = result;
      inst->a = index;
      inst->type = IR_TYPE_NUMBER;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    /* A definition is a store by the time this runs. The function only
     * lowers once it is hot, which is after the top level has already
     * defined its bindings — so the entry exists, and re-running the define
     * as a store to it is what the interpreter would do anyway. */
    case OP_DEFINE_GLOBAL:
    case OP_DEFINE_CONST:
    case OP_SET_GLOBAL:
    case OP_SET_GLOBAL_POP: {
      int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
      if (!csIrGlobalHoldsNumber(function, chunk, index)) return LOWER_HAND_OVER;
      if (low->stackTop < 1) {
        low->reason = "operand stack underflow while lowering";
        return LOWER_FAILED;
      }
      if (low->stack[low->stackTop - 1] < 0 || ir->registerTypes[low->stack[low->stackTop - 1]] != IR_TYPE_NUMBER) {
        return LOWER_HAND_OVER;
      }

      /* OP_SET_GLOBAL leaves the value; the other three consume it. */
      int value = opcode == OP_SET_GLOBAL ? low->stack[low->stackTop - 1] : csIrPop(low, block, line);
      if (low->reason != NULL) return LOWER_FAILED;

      IrInst *inst = csIrAppend(block, IR_STORE_GLOBAL, line);
      inst->a = index;
      inst->b = value;
      break;
    }

    case OP_GET_LOCAL: {
      int slot = chunk->code[offset + 1];
      int result = csIrNewRegister(ir, low->slotType[slot]);
      IrInst *inst = csIrAppend(block, IR_LOAD_LOCAL, line);
      inst->result = result;
      inst->a = slot;
      inst->type = low->slotType[slot];
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    case OP_GET_LOCAL_CONST: {
      int slot = chunk->code[offset + 1];
      int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
      int loaded = csIrNewRegister(ir, low->slotType[slot]);
      IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
      load->result = loaded;
      load->a = slot;
      load->type = low->slotType[slot];
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      if (!csIrPush(low, block, loaded, line)) return LOWER_FAILED;

      Value constant = chunk->constants.values[index];
      int result = csIrNewRegister(ir, IS_NUMBER(constant) ? IR_TYPE_NUMBER : IR_TYPE_UNKNOWN);
      IrInst *inst = csIrAppend(block, IR_CONST, line);
      inst->result = result;
      inst->constant = constant;
      inst->type = ir->registerTypes[result];
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    /* The fused pair from the superinstruction work: two loads, and the
     * lowering has to know about every one of them or it refuses on ordinary
     * code that happens to have been optimised. */
    case OP_GET_LOCAL_LOCAL: {
      for (int which = 1; which <= 2; which++) {
        int slot = chunk->code[offset + which];
        int result = csIrNewRegister(ir, low->slotType[slot]);
        IrInst *inst = csIrAppend(block, IR_LOAD_LOCAL, line);
        inst->result = result;
        inst->a = slot;
        inst->type = low->slotType[slot];
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      }
      break;
    }

    case OP_SET_LOCAL: {
      /* Stores and leaves the value: assignment is an expression. */
      int slot = chunk->code[offset + 1];
      int value = low->stackTop > 0 ? low->stack[low->stackTop - 1] : -1;
      if (value < 0) {
        low->reason = "operand stack underflow while lowering";
        return LOWER_FAILED;
      }
      IrInst *inst = csIrAppend(block, IR_STORE_LOCAL, line);
      inst->a = slot;
      inst->b = value;
      low->slotType[slot] = low->slotType[slot] == IR_TYPE_UNKNOWN && ir->registerTypes[value] != IR_TYPE_UNKNOWN
                                ? ir->registerTypes[value]
                                : (low->slotType[slot] == ir->registerTypes[value] ? low->slotType[slot] : IR_TYPE_UNKNOWN);
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      break;
    }

    case OP_TRUE:
    case OP_FALSE:
    case OP_NULL:
    case OP_UNDEFINED: {
      IrType type = (opcode == OP_TRUE || opcode == OP_FALSE) ? IR_TYPE_BOOL : IR_TYPE_UNKNOWN;
      int result = csIrNewRegister(ir, type);
      IrInst *inst = csIrAppend(block, IR_CONST, line);
      inst->result = result;
      inst->constant = opcode == OP_TRUE ? BOOL_VAL(true) : opcode == OP_FALSE ? BOOL_VAL(false) : opcode == OP_NULL ? NULL_VAL : UNDEFINED_VAL;
      inst->type = type;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    case OP_POP_N:
      for (int i = 0; i < chunk->code[offset + 1]; i++) {
        csIrPop(low, block, line);
        if (low->reason != NULL) return LOWER_FAILED;
      }
      break;

    case OP_DUP: {
      if (low->stackTop == 0) {
        low->reason = "operand stack underflow while lowering";
        return LOWER_FAILED;
      }
      if (!csIrPush(low, block, low->stack[low->stackTop - 1], line)) return LOWER_FAILED;
      break;
    }

    case OP_SET_LOCAL_POP: {
      int slot = chunk->code[offset + 1];
      int value = csIrPop(low, block, line);
      if (low->reason != NULL) return LOWER_FAILED;
      IrInst *inst = csIrAppend(block, IR_STORE_LOCAL, line);
      inst->a = slot;
      inst->b = value;
      low->slotType[slot] = low->slotType[slot] == IR_TYPE_UNKNOWN && ir->registerTypes[value] != IR_TYPE_UNKNOWN
                                ? ir->registerTypes[value]
                                : (low->slotType[slot] == ir->registerTypes[value] ? low->slotType[slot] : IR_TYPE_UNKNOWN);
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      break;
    }

    case OP_INC_LOCAL:
    case OP_DEC_LOCAL: {
      int slot = chunk->code[offset + 1];
      int loaded = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
      load->result = loaded;
      load->a = slot;

      int one = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *constant = csIrAppend(block, IR_CONST, line);
      constant->result = one;
      constant->constant = NUMBER_VAL(1);
      constant->type = IR_TYPE_NUMBER;

      int sum = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *add = csIrAppend(block, opcode == OP_INC_LOCAL ? IR_ADD : IR_SUB, line);
      add->result = sum;
      add->a = loaded;
      add->b = one;
      add->type = IR_TYPE_NUMBER;

      IrInst *store = csIrAppend(block, IR_STORE_LOCAL, line);
      store->a = slot;
      store->b = sum;
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      break;
    }

    case OP_POP:
      csIrPop(low, block, line);
      if (low->reason != NULL) return LOWER_FAILED;
      break;

    default: return LOWER_UNHANDLED;
  }
  return LOWER_OK;
}
