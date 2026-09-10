/* ir_build.c — the pieces the lowering builds with.
 *
 * Registers and instructions, where the block boundaries are, and the two
 * operations the whole abstract interpretation rests on: pushing a value onto
 * the modelled operand stack, and popping one off. In this VM a local *is* a
 * stack slot, so those two are also every read and write of a local.
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

/* ---- building ---------------------------------------------------------- */

int csIrNewRegister(IrFunction *ir, IrType type) {
  if (ir->registerCapacity < ir->registerCount + 1) {
    ir->registerCapacity = ir->registerCapacity < 16 ? 16 : ir->registerCapacity * 2;
    ir->registerTypes =
        (IrType *)realloc(ir->registerTypes, sizeof(IrType) * (size_t)ir->registerCapacity);
  }
  ir->registerTypes[ir->registerCount] = type;
  return ir->registerCount++;
}

IrInst *csIrAppend(IrBlock *block, IrOp op, int line) {
  if (block->capacity < block->count + 1) {
    block->capacity = block->capacity < 8 ? 8 : block->capacity * 2;
    block->instructions =
        (IrInst *)realloc(block->instructions, sizeof(IrInst) * (size_t)block->capacity);
  }
  IrInst *inst = &block->instructions[block->count++];
  memset(inst, 0, sizeof *inst);
  inst->op = op;
  inst->result = -1;
  inst->a = inst->b = inst->c = -1;
  inst->type = IR_TYPE_UNKNOWN;
  inst->line = line;
  return inst;
}

/* ---- finding the block boundaries -------------------------------------- */

/* Every jump target starts a block, and so does the instruction after a jump.
 * Both are found by reading the chunk once; the lowering then walks it again
 * knowing where the seams are. */
bool csIrMarkLeaders(const Chunk *chunk, bool *leader, const char **reason) {
  leader[0] = true;

  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    int next = csInstructionLength(chunk, offset);
    if (next <= offset) {
      *reason = "could not be decoded";
      return false;
    }

    switch (opcode) {
      case OP_JUMP:
      case OP_JUMP_IF_FALSE:
      case OP_JUMP_IF_TRUE:
      case OP_POP_JUMP_IF_FALSE:
      case OP_JUMP_IF_NOT_LESS:
      case OP_JUMP_IF_NOT_LESS_EQUAL:
      case OP_JUMP_IF_NOT_GREATER:
      case OP_JUMP_IF_NOT_GREATER_EQUAL:
      case OP_JUMP_IF_NOT_EQUAL:
      case OP_JUMP_IF_EQUAL: {
        int target = next + ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        if (target < chunk->count) leader[target] = true;
        if (next < chunk->count) leader[next] = true;
        break;
      }
      case OP_LOOP: {
        int target = next - ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        if (target >= 0) leader[target] = true;
        if (next < chunk->count) leader[next] = true;
        break;
      }
      default: break;
    }
    offset = next;
  }
  return true;
}

/* Whether the module already has a number under that name.
 *
 * Read at the moment the function turns hot, which is the only moment the
 * lowering has a running program to ask. What keeps the answer true
 * afterwards is checked again at every entry — see the guards on JitCode. */
bool csIrGlobalHoldsNumber(const ObjFunction *function, const Chunk *chunk,
                              int nameIndex) {
  if (function->module == NULL) return false;
  if (nameIndex < 0 || nameIndex >= chunk->constants.count) return false;

  Value name = chunk->constants.values[nameIndex];
  if (!IS_STRING(name)) return false;

  Value current;
  if (!csTableGet(&function->module->globals, AS_STRING(name), &current)) return false;
  return IS_NUMBER(current);
}

/* Whether the module already holds a callable under that name.
 *
 * The same question globalHoldsNumber asks, and answered at the same moment
 * and for the same reason: this is the only point at which the lowering has a
 * running program to consult. What keeps the answer true afterwards is
 * csIrInlinedCalleesHold, asked again at every entry. */
ObjClosure *csIrGlobalCallable(const ObjFunction *function, const Chunk *chunk,
                                  int nameIndex) {
  if (function->module == NULL) return NULL;
  if (nameIndex < 0 || nameIndex >= chunk->constants.count) return NULL;

  Value name = chunk->constants.values[nameIndex];
  if (!IS_STRING(name)) return NULL;

  Value current;
  if (!csTableGet(&function->module->globals, AS_STRING(name), &current)) return NULL;
  if (!IS_CLOSURE(current)) return NULL;
  return AS_CLOSURE(current);
}

void csIrClearPendingCallees(Lowering *low) {
  if (low->pendingCount == 0) return;
  for (int i = 0; i < IR_MAX_STACK; i++) low->pendingCallee[i] = NULL;
  low->pendingCount = 0;
}

/* Records that a block is also entered along a path the lowering did not walk:
 * the taken arm of a jump inside a run handed over to the interpreter.
 *
 * Merging rather than replacing, because the block may already have a state
 * recorded from the walk, and both are real paths in. The height is the one
 * thing a meet cannot fix — it decides which slot every emitted instruction
 * names — so a disagreement there is reported rather than reconciled. */
static void recordArrivalTypes(IrFunction *ir, int block, const IrType *slotType) {
  IrType *recorded = &ir->blockEntryTypes[(size_t)block * IR_MAX_SLOTS];
  if (!ir->blockEntrySeeded[block]) {
    memcpy(recorded, slotType, sizeof(IrType) * IR_MAX_SLOTS);
    ir->blockEntrySeeded[block] = true;
    return;
  }
  for (int s = 0; s < IR_MAX_SLOTS; s++) {
    if (recorded[s] != slotType[s]) recorded[s] = IR_TYPE_UNKNOWN;
  }
}

bool csIrRecordArrival(IrFunction *ir, Lowering *low, int block,
                          const IrType *slotType, int height) {
  if (block < 0 || block >= ir->blockCount || block >= IR_MAX_BLOCKS) return false;
  if (low->entryHeight[block] < 0) {
    low->entryHeight[block] = height;
  } else if (low->entryHeight[block] != height) {
    return false;
  }
  recordArrivalTypes(ir, block, slotType);
  return true;
}

/* Notes that `target` is entered with `height` on the stack, and with the
 * slots holding what they hold here.
 *
 * The types are recorded for the same reason a replayed jump's are: this is a
 * real path into that block, and where the walk picks up again after a run it
 * could not carry its own model across, what some predecessor recorded is the
 * only thing worth believing. Without it a loop below a `continue` inside a
 * handed-over run lost its counter's type and the whole function with it. */
bool csIrReachBlock(Lowering *low, int target, int height) {
  if (target < 0 || target >= IR_MAX_BLOCKS) return true;
  if (target < low->ir->blockCount) {
    recordArrivalTypes(low->ir, target, low->slotType);
  }
  if (low->entryHeight[target] < 0) {
    low->entryHeight[target] = height;
    return true;
  }
  if (low->entryHeight[target] != height) {
    low->reason = "operand stack height differs between paths into a block";
    return false;
  }
  return true;
}

/* The operand stack and the locals are the same array.
 *
 * `let total = 0` emits no instruction at all: the initialiser leaves its value
 * on the stack and the compiler simply calls that position a local from then
 * on. So a model with separate slots and temporaries is wrong — it loses every
 * local that was declared rather than stored.
 *
 * The IR therefore mirrors the frame exactly: pushing a value stores it at its
 * stack position, and popping loads it back. That is more instructions than
 * the bytecode had, and a later pass would keep short-lived values in
 * registers instead. Getting it right first is worth more than getting it
 * small: a wrong lowering is invisible until it produces a wrong number. */
bool csIrPush(Lowering *low, IrBlock *block, int reg, int line) {
  if (low->stackTop >= IR_MAX_STACK) {
    low->reason = "expression stack too deep";
    return false;
  }
  /* The frame base — the callee and the arguments — is held as -1, because no
   * instruction in this function produced it. Duplicating one of those would
   * mean storing a register that does not exist. */
  if (reg < 0) {
    low->reason = "a frame slot was used as a value";
    return false;
  }
  IrInst *store = csIrAppend(block, IR_STORE_LOCAL, line);
  store->a = low->stackTop;
  store->b = reg;
  low->slotType[low->stackTop] = low->ir->registerTypes[reg];
  if (low->stackTop + 1 > low->ir->slotCount) low->ir->slotCount = low->stackTop + 1;
  low->stack[low->stackTop++] = reg;
  return true;
}

int csIrPop(Lowering *low, IrBlock *block, int line) {
  if (low->stackTop == 0) {
    low->reason = "operand stack underflow while lowering";
    return 0;
  }
  low->stackTop--;
  int result = csIrNewRegister(low->ir, low->slotType[low->stackTop]);
  IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
  load->type = low->slotType[low->stackTop];
  load->result = result;
  load->a = low->stackTop;
  /* A block entered at a height recorded from a jump can read a position no
   * push in this function ever wrote, so the slot count grows here too. */
  if (low->stackTop + 1 > low->ir->slotCount) low->ir->slotCount = low->stackTop + 1;
  return result;
}

/* The block a bytecode offset belongs to, by linear search over the starts —
 * fine for the sizes involved, and it keeps the mapping in one place. */
int csIrBlockAt(const IrFunction *ir, int offset) {
  for (int i = 0; i < ir->blockCount; i++) {
    if (ir->blocks[i].bytecodeStart == offset) return i;
  }
  return -1;
}

bool csIrLowerBinary(Lowering *low, IrBlock *block, IrOp op, IrType type, int line) {
  int right = csIrPop(low, block, line);
  int left = csIrPop(low, block, line);
  if (low->reason != NULL) return false;

  int result = csIrNewRegister(low->ir, type);
  IrInst *inst = csIrAppend(block, op, line);
  inst->result = result;
  inst->a = left;
  inst->b = right;
  inst->type = type;
  return csIrPush(low, block, result, line);
}
