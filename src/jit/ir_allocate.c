/* ir_allocate.c — building an object from compiled code.
 *
 * The one thing compiled code does that can collect, and the reason the
 * collector had to be told where a compiled frame is. Three rules make it
 * sound, and each is an answer to something a previous attempt got wrong:
 *
 *   The object goes into its frame slot **before** its properties are put on
 *   it. The slot is inside the root range, so from that instant the collector
 *   can see it — and putting a property on it may allocate.
 *
 *   The values come from the slots above the destination, written by ordinary
 *   stores. They are numbers, so they need no rooting; taking them from the
 *   frame rather than from registers is what lets the emitted code be a call
 *   with two arguments instead of a list.
 *
 *   Nothing else may believe it knows what the destination slot holds. A slot
 *   written here is written by something that is not a store, and three passes
 *   reason about slots by looking for stores — see csIrWritesSlot, which is
 *   what they all ask now.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/object_ops.h"
#include "cscript/shape.h"
#include "cscript/vm.h"
#include "jit/ir_internal.h"

/* Called from compiled code. `slots` is the frame, which the collector is
 * walking for the length of the run; the values are in the slots above the
 * destination, one per key. */
void csJitBuildObject(Value *slots, int destination, const IrObjectLiteral *literal) {
  /* The same name a literal gets in the interpreter: what an object prints
   * as, and not a property of it. */
  ObjObject *object = csObjectNew("Object");

  /* Into the frame first: everything below this line may allocate, and this
   * slot is what the collector will see the object through. */
  slots[destination] = OBJ_VAL(object);

  /* Room for what is about to go on, so that no put has to grow the storage
   * mid-way — the same reservation an instance is built with. */
  csObjectReserveSlots(object, literal->count);

  for (int i = 0; i < literal->count; i++) {
    csObjectPut(object, literal->keys[i], slots[destination + 1 + i]);
  }
}

/* Records one literal's keys, answering the index compiled code will use.
 * Answers -1 when there is no room or a key is not a plain string, which is
 * what the lowering treats as "not this one". */
int csIrAddLiteral(IrFunction *ir, ObjString **keys, int count) {
  if (count <= 0 || count > IR_MAX_LITERAL_KEYS) return -1;
  for (int i = 0; i < count; i++) {
    if (keys[i] == NULL) return -1;
  }

  if (ir->literalCount == ir->literalCapacity) {
    int capacity = ir->literalCapacity < 4 ? 4 : ir->literalCapacity * 2;
    IrObjectLiteral *grown = (IrObjectLiteral *)realloc(ir->literals, sizeof(IrObjectLiteral) * (size_t)capacity);
    if (grown == NULL) return -1;
    ir->literals = grown;
    ir->literalCapacity = capacity;
  }

  IrObjectLiteral *literal = &ir->literals[ir->literalCount];
  literal->count = count;
  for (int i = 0; i < count; i++) literal->keys[i] = keys[i];
  return ir->literalCount++;
}

/* Which frame slot an instruction writes, or -1.
 *
 * A store writes the slot it names. So does an allocation, and that is the
 * whole point of this function: the passes that decide what a slot holds, what
 * may live in a register, and which stores are dead all used to look for
 * IR_STORE_LOCAL alone — so a slot written by an allocation looked untouched.
 * It was then typed a number, given a floating-point register, and read back
 * as whatever had been there before. */
int csIrWritesSlot(const IrInst *inst) {
  if (inst->op == IR_STORE_LOCAL) return inst->a;
  if (inst->op == IR_NEW_OBJECT) return inst->a;
  return -1;
}

/* And which slots it reads without a load to show for it: the object a
 * property is reached through, and the values an allocation collects. */
void csIrReadsSlots(const IrFunction *ir, const IrInst *inst, int *first, int *last) {
  *first = -1;
  *last = -1;
  switch (inst->op) {
    case IR_LOAD_PROPERTY:
    case IR_STORE_PROPERTY:
    case IR_ADD_PROPERTY:
      *first = inst->a;
      *last = inst->a;
      return;

    case IR_NEW_OBJECT: {
      if (inst->b < 0 || inst->b >= ir->literalCount) return;
      *first = inst->a + 1;
      *last = inst->a + ir->literals[inst->b].count;
      return;
    }

    default: return;
  }
}
