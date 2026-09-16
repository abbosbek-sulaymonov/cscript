/* ir_lower_object.c — property reads and writes, and the layouts they
 * assume —
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

/* Does any instruction in this chunk store to `slot`?
 *
 * Asked of the bytecode rather than of the IR because the answer has to be
 * known while the IR is still being built. A parameter that is never assigned
 * is the common case and the only one this admits. */
static bool slotIsNeverWritten(const Chunk *chunk, int slot) {
  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    if ((opcode == OP_SET_LOCAL || opcode == OP_SET_LOCAL_POP) && chunk->code[offset + 1] == slot) {
      return false;
    }
    int next = csInstructionLength(chunk, offset);
    if (next <= offset) return false;
    offset = next;
  }
  return true;
}

/* Records what a property read takes for granted, merging with an assumption
 * already made about the same slot and property. Two reads of the same field
 * cost one check. */
static bool rememberEntryShape(IrFunction *ir, int slot, Shape *shape, int property, bool expectsNumber, int minimumCapacity) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    IrEntryShape *existing = &ir->entryShapes[i];
    if (existing->slot != slot) continue;
    /* One slot, one layout *on the way in*. A second shape for the same slot
     * would mean the site is not monomorphic after all — an add is not a
     * second shape here, because what it expects is still the entry one and
     * what it produces is carried by the instruction. */
    if (existing->shape != shape) return false;
    if (minimumCapacity > existing->minimumCapacity) existing->minimumCapacity = minimumCapacity;
    if (existing->property == property) {
      /* Read and written both: the read's requirement is the stricter one. */
      existing->expectsNumber = existing->expectsNumber || expectsNumber;
      return true;
    }
    if (property < 0) return true; /* a capacity requirement and nothing more */
  }

  if (ir->entryShapeCount == ir->entryShapeCapacity) {
    int capacity = ir->entryShapeCapacity < 4 ? 4 : ir->entryShapeCapacity * 2;
    IrEntryShape *grown = (IrEntryShape *)realloc(ir->entryShapes, sizeof(IrEntryShape) * (size_t)capacity);
    if (grown == NULL) return false;
    ir->entryShapes = grown;
    ir->entryShapeCapacity = capacity;
  }

  ir->entryShapes[ir->entryShapeCount].slot = slot;
  ir->entryShapes[ir->entryShapeCount].shape = shape;
  ir->entryShapes[ir->entryShapeCount].property = property;
  ir->entryShapes[ir->entryShapeCount].expectsNumber = expectsNumber;
  ir->entryShapes[ir->entryShapeCount].minimumCapacity = minimumCapacity;
  ir->entryShapeCount++;
  return true;
}

/* Raises how much room a slot's object must already have. The record itself
 * was made by the first store on that slot; this is every one after it. */
static bool raiseEntryCapacity(IrFunction *ir, int slot, int needed) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    if (ir->entryShapes[i].slot != slot) continue;
    if (needed > ir->entryShapes[i].minimumCapacity) ir->entryShapes[i].minimumCapacity = needed;
    return true;
  }
  return false;
}

bool csIrEntryShapesHold(const IrFunction *ir, const Value *slots) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    const IrEntryShape *assumed = &ir->entryShapes[i];
    Value held = slots[assumed->slot];
    if (!IS_OBJECT(held)) return false;

    ObjObject *object = AS_OBJECT(held);
    if (object->shape != assumed->shape) return false;
    /* Room for every property the body adds. An add in compiled code is two
     * stores; growing the storage is an allocation it cannot make, so the
     * question is asked once here rather than at each store. */
    if (object->as.slots.capacity < assumed->minimumCapacity) return false;
    if (assumed->property >= 0 && assumed->property >= object->shape->slotCount) return false;
    if (assumed->expectsNumber && !IS_NUMBER(object->as.slots.values[assumed->property])) {
      return false;
    }
  }
  return true;
}

LowerResult csIrLowerObject(LowerAt *at) {
  Lowering *low = at->low;
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  const int line = at->line;
  const Chunk *chunk = at->chunk;
  const int offset = at->offset;
  const uint8_t opcode = chunk->code[offset];

  switch (opcode) {
    /* `object.name = value`, with the same conditions the read needs plus
     * one of its own: the value stored has to be a number, because that is
     * the only form the compiled code holds a value in.
     *
     * The property already exists in the shape the cache names — that is
     * what a cache hit means — so the write cannot grow the object or tip it
     * into dictionary mode, which is what makes an unguarded store sound. */
    case OP_SET_PROPERTY:
    case OP_SET_PROPERTY_POP: {
      int cacheIndex = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
      if (low->stackTop < 2 || cacheIndex >= chunk->propertyCacheCount) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      int value = low->stack[low->stackTop - 1];
      int object = low->stack[low->stackTop - 2];
      if (value < 0 || object < 0 || ir->registerTypes[value] != IR_TYPE_NUMBER) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      int slot = -1;
      for (int i = block->count - 1; i >= 0; i--) {
        if (block->instructions[i].result != object) continue;
        if (block->instructions[i].op == IR_LOAD_LOCAL) slot = block->instructions[i].a;
        break;
      }

      const PropertyCache *cache = &chunk->propertyCaches[cacheIndex];
      if (slot < 0 || cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || !slotIsNeverWritten(chunk, slot) || slot >= IR_MAX_SLOTS) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      /* A store that *adds* — what a constructor does. The site's cache holds
       * the pair it saw: the layout on the way in, and the one the object
       * takes on. What has to hold here is that the object really has that
       * first layout at this point, which is the entry shape for the first add
       * and whatever the previous add produced for the ones after it.
       *
       * The room for the value is checked once at entry rather than here,
       * because an add with nowhere to put the value would have to grow the
       * storage, and growing it is an allocation compiled code cannot make. */
      bool adds = cache->added != NULL;
      Shape *expected = low->slotShape[slot] != NULL ? low->slotShape[slot] : cache->shape;
      if (adds && cache->shape != expected) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      /* The entry requirement is the layout the *first* store on this slot
       * expected, and room for every property added to it. A later store in
       * the chain has nothing new to say about the entry layout — only about
       * how much room the storage needs. */
      int needed = adds ? cache->added->slotCount : 0;
      bool recorded =
          low->slotShape[slot] != NULL ? raiseEntryCapacity(ir, slot, needed) : rememberEntryShape(ir, slot, cache->shape, adds ? -1 : cache->slot, false, needed);
      if (!recorded) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      csIrPop(low, block, line); /* the value */
      csIrPop(low, block, line); /* the object */

      IrInst *inst = csIrAppend(block, adds ? IR_ADD_PROPERTY : IR_STORE_PROPERTY, line);
      if (adds) {
        inst->constant = OBJ_VAL(cache->added);
        low->slotShape[slot] = cache->added;
      }
      inst->result = -1;
      inst->a = slot;
      /* The value goes in `b` because that is where every other store keeps
       * one, and csIrRegisterOperands answers `b` for those. Putting it in
       * `c` made it invisible to every pass that asks which fields are
       * registers — so the value would have looked dead and been eliminated
       * out from under the store. */
      inst->b = value;
      inst->c = cache->slot;
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;

      /* `obj.x = v` is an expression and leaves the value; the _POP form is
       * the same store in statement position and leaves nothing. */
      if (opcode == OP_SET_PROPERTY && !csIrPush(low, block, value, line)) return LOWER_FAILED;
      break;
    }

    /* `object.name`, where the object came straight off a frame slot.
     *
     * The fused form below names its slot in the instruction; this one does
     * not, so the slot is recovered from whatever pushed the value — and the
     * only producer this accepts is a plain load. Anything else and the
     * object is a computed value whose layout at entry says nothing about
     * its layout here.
     *
     * Worth the second case because a method's `this.x` compiles to this
     * form rather than the fused one, and a method is where property reads
     * mostly live. */
    case OP_GET_PROPERTY: {
      int cacheIndex = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
      if (low->stackTop < 1 || cacheIndex >= chunk->propertyCacheCount) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      int object = low->stack[low->stackTop - 1];
      if (object < 0) return LOWER_HAND_OVER;
      int slot = -1;
      for (int i = block->count - 1; i >= 0; i--) {
        if (block->instructions[i].result != object) continue;
        if (block->instructions[i].op == IR_LOAD_LOCAL) slot = block->instructions[i].a;
        break;
      }

      const PropertyCache *cache = &chunk->propertyCaches[cacheIndex];
      if (slot < 0 || cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || !slotIsNeverWritten(chunk, slot) ||
          !rememberEntryShape(ir, slot, cache->shape, cache->slot, true, 0)) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      csIrPop(low, block, line);
      int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *inst = csIrAppend(block, IR_LOAD_PROPERTY, line);
      inst->result = result;
      inst->a = slot;
      inst->b = cache->slot;
      inst->type = IR_TYPE_NUMBER;
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    /* `local.name`, where the site has settled on one layout.
     *
     * Three things have to hold, and all three are checkable here. The site
     * must be monomorphic — a cache that has seen one shape is the profile
     * this needs, and it is already being kept. The slot must be one the
     * function never writes, or the object checked at entry would not be the
     * object read here. And the property must hold a number, because
     * anything else has no unboxed form and nothing downstream could use
     * it. */
    case OP_GET_LOCAL_PROPERTY: {
      int slot = chunk->code[offset + 1];
      int cacheIndex = (chunk->code[offset + 4] << 8) | chunk->code[offset + 5];
      if (cacheIndex >= chunk->propertyCacheCount) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      const PropertyCache *cache = &chunk->propertyCaches[cacheIndex];
      if (cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || !slotIsNeverWritten(chunk, slot)) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      if (!rememberEntryShape(ir, slot, cache->shape, cache->slot, true, 0)) {
        low->reason = csOpcodeName((OpCode)opcode);
        return LOWER_FAILED;
      }

      int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
      IrInst *inst = csIrAppend(block, IR_LOAD_PROPERTY, line);
      inst->result = result;
      inst->a = slot;
      inst->b = cache->slot;
      inst->type = IR_TYPE_NUMBER;
      if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
      break;
    }

    default: return LOWER_UNHANDLED;
  }
  return LOWER_OK;
}
