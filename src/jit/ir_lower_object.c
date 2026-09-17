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

/* The instruction that produced a register, searched backwards in the block
 * it was produced in. A key is pushed by the instruction just before the one
 * that reads it, so the search is short — and a register produced in another
 * block is not one this can answer for, which is the conservative answer. */
static const IrInst *definitionOf(const IrBlock *block, int reg) {
  for (int i = block->count - 1; i >= 0; i--) {
    if (block->instructions[i].result == reg) return &block->instructions[i];
  }
  return NULL;
}

/* Records what a property read takes for granted, merging with an assumption
 * already made about the same slot and property. Two reads of the same field
 * cost one check. */
/* Room for one more record in the table both kinds share. Answers its index,
 * zeroed, or -1. */
static int takeShapeRecord(IrFunction *ir) {
  if (ir->entryShapeCount == ir->entryShapeCapacity) {
    int capacity = ir->entryShapeCapacity < 4 ? 4 : ir->entryShapeCapacity * 2;
    IrEntryShape *grown = (IrEntryShape *)realloc(ir->entryShapes, sizeof(IrEntryShape) * (size_t)capacity);
    if (grown == NULL) return -1;
    ir->entryShapes = grown;
    ir->entryShapeCapacity = capacity;
  }
  IrEntryShape *record = &ir->entryShapes[ir->entryShapeCount];
  memset(record, 0, sizeof *record);
  return ir->entryShapeCount++;
}

/* A check at the property site, for what an entry assumption cannot say.
 *
 * Answers the record's index, which the instruction carries, or -1. Two reads
 * of the same property at the same offset share one — the same merge the entry
 * records do, and for the same reason. */
static int rememberSiteGuard(IrFunction *ir, int slot, Shape *shape, int property, int offset, int height) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    IrEntryShape *existing = &ir->entryShapes[i];
    if (!existing->atSite || existing->slot != slot || existing->shape != shape) continue;
    if (existing->deoptOffset != offset) continue;
    existing->numberSlots |= 1u << property;
    return i;
  }

  int index = takeShapeRecord(ir);
  if (index < 0) return -1;
  IrEntryShape *record = &ir->entryShapes[index];
  record->slot = slot;
  record->shape = shape;
  /* A site guard carries its requirements in the mask, not in `property`: one
   * guard answers for every read that follows it. */
  record->property = -1;
  record->numberSlots = 1u << property;
  record->atSite = true;
  record->deoptOffset = offset;
  record->deoptHeight = height;
  return index;
}

static bool rememberEntryShape(IrFunction *ir, int slot, Shape *shape, int property, bool expectsNumber, int minimumCapacity) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    IrEntryShape *existing = &ir->entryShapes[i];
    if (existing->atSite || existing->slot != slot) continue;
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

  int index = takeShapeRecord(ir);
  if (index < 0) return false;
  IrEntryShape *record = &ir->entryShapes[index];
  record->slot = slot;
  record->shape = shape;
  record->property = property;
  record->expectsNumber = expectsNumber;
  record->minimumCapacity = minimumCapacity;
  return true;
}

/* Raises how much room a slot's object must already have. The record itself
 * was made by the first store on that slot; this is every one after it. */
static bool raiseEntryCapacity(IrFunction *ir, int slot, int needed) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    if (ir->entryShapes[i].atSite || ir->entryShapes[i].slot != slot) continue;
    if (needed > ir->entryShapes[i].minimumCapacity) ir->entryShapes[i].minimumCapacity = needed;
    return true;
  }
  return false;
}

/* One record, asked of one frame. Shared by the entry check and the guard the
 * compiled code calls, so the two can never come to disagree about what an
 * assumption means. */
static bool shapeRecordHolds(const IrEntryShape *assumed, const Value *slots) {
  Value held = slots[assumed->slot];
  if (!IS_OBJECT(held)) return false;

  ObjObject *object = AS_OBJECT(held);
  if (object->shape != assumed->shape) return false;
  /* Room for every property the body adds. An add in compiled code is two
   * stores; growing the storage is an allocation it cannot make, so the
   * question is asked once rather than at each store. */
  if (object->as.slots.capacity < assumed->minimumCapacity) return false;

  /* A site guard stands for every read that follows it, and those read
   * different properties. The layout alone proves nothing about the values:
   * two objects of one shape can hold a number in one slot and a string in the
   * next. So every property the reads will take as a number is asked for. */
  if (assumed->atSite) {
    for (int index = 0; index < 32; index++) {
      if (((assumed->numberSlots >> index) & 1u) == 0) continue;
      if (index >= object->shape->slotCount) return false;
      if (!IS_NUMBER(object->as.slots.values[index])) return false;
    }
    return true;
  }

  if (assumed->property >= 0 && assumed->property >= object->shape->slotCount) return false;
  if (assumed->expectsNumber && !IS_NUMBER(object->as.slots.values[assumed->property])) return false;
  return true;
}

/* Called from compiled code, once per guarded read. False means deoptimise. */
bool csJitShapeHolds(const Value *slots, const IrEntryShape *guard) {
  return shapeRecordHolds(guard, slots);
}

bool csIrEntryShapesHold(const IrFunction *ir, const Value *slots) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    const IrEntryShape *assumed = &ir->entryShapes[i];
    /* A site guard says nothing about the frame on the way in — that is the
     * whole of why it is at the site. */
    if (assumed->atSite) continue;
    if (!shapeRecordHolds(assumed, slots)) return false;
  }
  return true;
}

/* Has anything lowered so far put a value in this slot?
 *
 * The bytecode question — slotIsNeverWritten — is about assignment, and misses
 * the case that matters most: `const p = { … }` assigns nothing, because a
 * local *is* its stack position, so a slot holding an object the body built
 * looks untouched. What is asked here is what an entry assumption actually
 * needs to be false, and the IR is where the answer is: an allocation, a call
 * and a push all write a slot, and csIrWritesSlot names all three.
 *
 * "So far" is the whole of it, and that is not a gap. The two ways a slot gets
 * a value are a declaration and an assignment; assignment is what the bytecode
 * question already covers, and a declaration is lowered before every read of
 * what it declares. So a write this has not seen yet cannot be the source of
 * the value the read is about to take — the only other source is the caller,
 * which is exactly what an entry assumption checks. */
static bool slotWrittenSoFar(const IrFunction *ir, int slot) {
  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      if (csIrWritesSlot(&ir->blocks[b].instructions[i]) == slot) return true;
    }
  }
  return false;
}

/* The record of a guard earlier in this block that already proved this slot
 * holds an object of this layout, with nothing written to the slot since — or
 * -1. Answering the record rather than a yes is what lets the read that reuses
 * it add its own property to what that guard asks for. */
static int reusableGuard(const Lowering *low, const IrBlock *block, int slot, const Shape *shape) {
  if (slot < 0 || slot >= IR_MAX_SLOTS || low->slotGuard[slot] != shape) return -1;
  for (int i = low->slotGuardAt[slot]; i < block->count; i++) {
    if (csIrWritesSlot(&block->instructions[i]) == slot) return -1;
  }
  return low->slotGuardRecord[slot];
}

/* Emits a read of `slot`.`property`, guarding at the site when an entry
 * assumption cannot speak for it — and answering false when neither route is
 * open, which is a hand-over rather than a refusal.
 *
 * Two things stop an entry assumption. A slot the body *writes* holds nothing
 * at entry worth checking: an object built inside the function is the clearest
 * case, and reading a property of one failed the entry check on every single
 * call, so the whole call was interpreted for the sake of a check that could
 * never have held. And a slot that already has a layout recorded cannot take a
 * second one — one of the two sites has to ask for itself. */
static bool lowerPropertyRead(LowerAt *at, int slot, const PropertyCache *cache, int height) {
  Lowering *low = at->low;
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  const int line = at->line;

  bool checked = slotIsNeverWritten(at->chunk, slot) && !slotWrittenSoFar(ir, slot) && rememberEntryShape(ir, slot, cache->shape, cache->slot, true, 0);

  if (!checked) {
    /* Two reads of the same object cost one check: the guard already emitted
     * takes this read's property on as well, and asks for both. */
    int reused = reusableGuard(low, block, slot, cache->shape);
    if (reused >= 0) {
      ir->entryShapes[reused].numberSlots |= 1u << cache->slot;
      checked = true;
    }
  }

  if (!checked) {
    /* The interpreter picks the frame up at this instruction and does the read
     * its own way, so the height is the one the instruction started at and
     * nothing of it has happened yet. */
    int guard = rememberSiteGuard(ir, slot, cache->shape, cache->slot, at->offset, height);
    if (guard < 0) return false;

    IrInst *check = csIrAppend(block, IR_GUARD_SHAPE, line);
    check->result = -1;
    check->a = slot;
    check->b = guard;
    ir->hasExits = true;

    low->slotGuard[slot] = cache->shape;
    low->slotGuardAt[slot] = block->count;
    low->slotGuardRecord[slot] = guard;
  }

  int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
  IrInst *inst = csIrAppend(block, IR_LOAD_PROPERTY, line);
  inst->result = result;
  inst->a = slot;
  inst->b = cache->slot;
  inst->type = IR_TYPE_NUMBER;
  if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
  return csIrPush(low, block, result, line);
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

      /* Everything below is a reason this one store cannot be compiled, and
       * none of them is a reason to refuse the function: nothing has been
       * popped yet, so the frame at this instruction is one the interpreter
       * can pick up and redo the store from. */

      int value = low->stack[low->stackTop - 1];
      int object = low->stack[low->stackTop - 2];
      if (value < 0 || object < 0 || ir->registerTypes[value] != IR_TYPE_NUMBER) return LOWER_HAND_OVER;

      int slot = -1;
      for (int i = block->count - 1; i >= 0; i--) {
        if (block->instructions[i].result != object) continue;
        if (block->instructions[i].op == IR_LOAD_LOCAL) slot = block->instructions[i].a;
        break;
      }

      const PropertyCache *cache = &chunk->propertyCaches[cacheIndex];
      if (slot < 0 || cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || !slotIsNeverWritten(chunk, slot) || slot >= IR_MAX_SLOTS) {
        return LOWER_HAND_OVER;
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
      if (adds && cache->shape != expected) return LOWER_HAND_OVER;

      /* The entry requirement is the layout the *first* store on this slot
       * expected, and room for every property added to it. A later store in
       * the chain has nothing new to say about the entry layout — only about
       * how much room the storage needs. */
      int needed = adds ? cache->added->slotCount : 0;
      bool recorded =
          low->slotShape[slot] != NULL ? raiseEntryCapacity(ir, slot, needed) : rememberEntryShape(ir, slot, cache->shape, adds ? -1 : cache->slot, false, needed);
      if (!recorded) return LOWER_HAND_OVER;

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

    /* `{ x: 1, y: 2 }` — the one instruction here that allocates.
     *
     * The keys are constants pushed just before it and the values are whatever
     * produced them, so what has to be true is that each key really is a
     * string constant and each value really is a number. The values are then
     * stored into the slots above the destination, and a call builds the
     * object from there.
     *
     * The destination is where the object lands on the operand stack, which is
     * a frame slot like any other: a local *is* its stack position, which is
     * what lets the property reads below reach it by slot. */
    case OP_OBJECT: {
      int count = chunk->code[offset + 1];
      if (count <= 0 || count > IR_MAX_LITERAL_KEYS || low->stackTop < count * 2) return LOWER_HAND_OVER;

      int destination = low->stackTop - count * 2;
      if (destination < 0 || destination + count >= IR_MAX_SLOTS) return LOWER_HAND_OVER;

      /* The pairs, bottom first: a key that is not a plain string constant, or
       * a value that is not known to be a number, and this is not a literal
       * the compiler can build. */
      ObjString *keys[IR_MAX_LITERAL_KEYS];
      int values[IR_MAX_LITERAL_KEYS];
      for (int i = 0; i < count; i++) {
        int keyRegister = low->stack[destination + i * 2];
        int valueRegister = low->stack[destination + i * 2 + 1];
        if (keyRegister < 0 || valueRegister < 0 || ir->registerTypes[valueRegister] != IR_TYPE_NUMBER) return LOWER_HAND_OVER;

        const IrInst *producer = definitionOf(block, keyRegister);
        if (producer == NULL || producer->op != IR_CONST || !IS_STRING(producer->constant)) return LOWER_HAND_OVER;
        keys[i] = AS_STRING(producer->constant);
        values[i] = valueRegister;
      }

      int literal = csIrAddLiteral(ir, keys, count);
      if (literal < 0) return LOWER_HAND_OVER;

      for (int i = 0; i < count * 2; i++) csIrPop(low, block, line);

      /* Each value into the slot the builder will read it from. Ordinary
       * stores, so every pass that reasons about slots sees them. */
      for (int i = 0; i < count; i++) {
        IrInst *store = csIrAppend(block, IR_STORE_LOCAL, line);
        store->result = -1;
        store->a = destination + 1 + i;
        store->b = values[i];
      }

      IrInst *inst = csIrAppend(block, IR_NEW_OBJECT, line);
      inst->result = -1;
      inst->a = destination;
      inst->b = literal;
      if (destination + count + 1 > ir->slotCount) ir->slotCount = destination + count + 1;

      /* The object is in its slot; what the stack holds is a read of it. */
      int result = csIrNewRegister(ir, IR_TYPE_UNKNOWN);
      IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
      load->result = result;
      load->a = destination;
      load->type = IR_TYPE_UNKNOWN;
      if (!csIrPush(low, block, result, line)) return LOWER_FAILED;
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

      /* A cache that saw the property *absent* names no layout to read from:
       * what it found was on a prototype, or nowhere. Nothing here can compile
       * that, and the frame goes back to the interpreter at this instruction
       * rather than the function being refused for it — which is what used to
       * happen, and what kept twenty of the corpus's hot functions out of the
       * compiler entirely. */
      const PropertyCache *cache = &chunk->propertyCaches[cacheIndex];
      /* The mask a site guard carries is 32 bits wide; a storage index past it
       * is one nothing here can speak for. */
      if (slot < 0 || cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || cache->slot >= 32 || slot >= IR_MAX_SLOTS) {
        return LOWER_HAND_OVER;
      }

      /* The height a guard resumes at is the one this instruction started at:
       * the interpreter finds the object still on the stack and does the read
       * its own way. So it is taken before the pop. */
      int height = low->stackTop;
      csIrPop(low, block, line);
      if (!lowerPropertyRead(at, slot, cache, height)) {
        return low->reason != NULL ? LOWER_FAILED : LOWER_HAND_OVER;
      }
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
      if (cache->shape == NULL || cache->shape == vm.absentShape || cache->slot < 0 || cache->slot >= 32 || slot >= IR_MAX_SLOTS) {
        return LOWER_HAND_OVER;
      }

      if (!lowerPropertyRead(at, slot, cache, low->stackTop)) {
        return low->reason != NULL ? LOWER_FAILED : LOWER_HAND_OVER;
      }
      break;
    }

    default: return LOWER_UNHANDLED;
  }
  return LOWER_OK;
}
