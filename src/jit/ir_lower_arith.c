/* ir_lower_arith.c — arithmetic and comparison, on operands proved to
 * be numbers —
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

LowerResult csIrLowerArith(LowerAt *at) {
  Lowering *low = at->low;
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  const int line = at->line;
  const Chunk *chunk = at->chunk;
  const int offset = at->offset;
  const uint8_t opcode = chunk->code[offset];

  switch (opcode) {
    case OP_ADD:
    case OP_ADD_NUM:
      /* Both are addition here. OP_ADD_NUM carries the checker's proof, and
       * plain OP_ADD only reaches this point at all when the walk in ir.c has
       * seen both operands typed as numbers — which is a proof of the same
       * thing, arrived at from the IR rather than from an annotation. A `+`
       * that might concatenate handed the frame back instead. */
      if (!csIrLowerBinary(low, block, IR_ADD, IR_TYPE_NUMBER, line)) return LOWER_FAILED;
      break;

    /* The rest of the arithmetic requires numbers by definition — the VM
     * errors otherwise — so the result type is known without an annotation. */
    case OP_SUBTRACT:
      if (!csIrLowerBinary(low, block, IR_SUB, IR_TYPE_NUMBER, line)) return LOWER_FAILED;
      break;
    case OP_MULTIPLY:
      if (!csIrLowerBinary(low, block, IR_MUL, IR_TYPE_NUMBER, line)) return LOWER_FAILED;
      break;
    case OP_DIVIDE:
      if (!csIrLowerBinary(low, block, IR_DIV, IR_TYPE_NUMBER, line)) return LOWER_FAILED;
      break;
    case OP_MODULO:
      if (!csIrLowerBinary(low, block, IR_MOD, IR_TYPE_NUMBER, line)) return LOWER_FAILED;
      break;

    case OP_LESS:
      if (!csIrLowerBinary(low, block, IR_LT, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;
    case OP_LESS_EQUAL:
      if (!csIrLowerBinary(low, block, IR_LE, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;
    case OP_GREATER:
      if (!csIrLowerBinary(low, block, IR_GT, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;
    case OP_GREATER_EQUAL:
      if (!csIrLowerBinary(low, block, IR_GE, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;
    case OP_EQUAL:
      if (!csIrLowerBinary(low, block, IR_EQ, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;
    case OP_NOT_EQUAL:
      if (!csIrLowerBinary(low, block, IR_NE, IR_TYPE_BOOL, line)) return LOWER_FAILED;
      break;

    case OP_NEGATE: {
      int operand = csIrPop(low, block, line);
      if (low->reason != NULL) return LOWER_FAILED;
      int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *inst = csIrAppend(block, IR_NEG, line);
      inst->result = result;
      inst->a = operand;
      inst->type = IR_TYPE_NUMBER;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    default:
      return LOWER_UNHANDLED;
  }
  return LOWER_OK;
}
