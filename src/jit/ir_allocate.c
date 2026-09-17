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
#include "cscript/type.h"
#include "cscript/vm.h"
#include "jit/ir_internal.h"
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
  if (inst->op == IR_CALL) return inst->a;
  return -1;
}

/* What a call answers, as a type: a number where the callee declared one, and
 * otherwise a value the compiled code may move but do no arithmetic to.
 *
 * The declaration is the whole of the proof. A call site sees one closure —
 * checked at entry, like an inlined one — and that closure's function said
 * what it returns, so the caller's arithmetic can be compiled on the strength
 * of it. Without this the call is lowered and then the function is refused for
 * adding an untyped value to a number, which is the shape of a feature that
 * technically works. */
IrType csIrCallResultType(const IrFunction *ir, int site) {
  if (site < 0 || site >= ir->callCount) return IR_TYPE_UNKNOWN;
  const ObjFunction *callee = ir->calls[site].callee->function;
  return callee->returnType == (uint8_t)TYPE_NUMBER ? IR_TYPE_NUMBER : IR_TYPE_UNKNOWN;
}

/* What an instruction leaves in the slot it writes. */
IrType csIrWrittenType(const IrFunction *ir, const IrInst *inst) {
  if (inst->op == IR_CALL) return csIrCallResultType(ir, inst->b);
  if (inst->op == IR_STORE_LOCAL && inst->b >= 0 && inst->b <= ir->registerCount) return ir->registerTypes[inst->b];
  return IR_TYPE_UNKNOWN;
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
    /* A guard reads the object out of the frame, the same way, and for the
     * same reason it must stay there: the check is a C function handed the
     * slots, and a value living in a register is not in the frame at all. */
    case IR_GUARD_SHAPE:
      *first = inst->a;
      *last = inst->a;
      return;

    case IR_NEW_OBJECT: {
      if (inst->b < 0 || inst->b >= ir->literalCount) return;
      *first = inst->a + 1;
      *last = inst->a + ir->literals[inst->b].count;
      return;
    }

    /* A call reads its arguments from the slots above the destination, the
     * same way a literal reads its values. */
    case IR_CALL: {
      if (inst->b < 0 || inst->b >= ir->callCount) return;
      *first = inst->a + 1;
      *last = inst->a + ir->calls[inst->b].argCount;
      return;
    }

    default: return;
  }
}

/* --- calling out --------------------------------------------------------- */

/* Calls a closure the way the interpreter would: the callee and its arguments
 * pushed, and the ordinary call machinery from there — so the callee may be
 * interpreted, compiled, or compiled and entered through its own guard, and
 * this does not have to know which.
 *
 * The arguments are read out of the frame rather than taken as C arguments,
 * for the same reason the values of an object literal are: they are already
 * there, the frame is what the collector is walking, and a call with a fixed
 * shape is one the emitter can make without a list.
 *
 * Answers false when the callee threw or failed. Compiled code has no handler
 * and cannot have one — a `try` in the caller was never lowered — so what it
 * does with a false is leave, and the frame fails as it would have had the
 * interpreter made the call. */
bool csJitCallClosure(Value *slots, int destination, const IrCallSite *site) {
  csVMPush(OBJ_VAL(site->callee));
  for (int i = 0; i < site->argCount; i++) {
    csVMPush(slots[destination + 1 + i]);
  }

  Value answered;
  if (!csVMCallFromCompiled(OBJ_VAL(site->callee), site->argCount, &answered)) return false;

  /* Into the frame, where the interpreter leaves a call's result and where the
   * collector can see it: the operand stack a compiled function runs on *is*
   * these slots. */
  slots[destination] = answered;
  return true;
}

/* Lowers a call the splice would not take.
 *
 * The arguments go into the slots above the callee's position — where the
 * interpreter would have left them, because the operand stack is the frame —
 * and the result comes back into the callee's own position, which is where the
 * interpreter leaves one. So the frame after the call looks exactly as it
 * would have, and an exit anywhere downstream needs nothing put back.
 *
 * Answers the register holding the result, or -1. */
int csIrLowerRealCall(LowerAt *at, ObjClosure *closure, int base, const int *args, int argCount) {
  IrFunction *ir = at->ir;
  IrBlock *block = at->block;
  Lowering *low = at->low;
  const int line = at->line;

  if (base < 0 || base + argCount + 1 >= IR_MAX_SLOTS) return -1;
  if (low->pendingName[base] < 0) return -1;

  ObjString *name = AS_STRING(at->chunk->constants.values[low->pendingName[base]]);
  int site = csIrAddCall(ir, &at->function->module->globals, name, closure, argCount);
  if (site < 0) return -1;

  for (int a = 0; a < argCount; a++) {
    if (args[a] < 0) return -1;
    IrInst *store = csIrAppend(block, IR_STORE_LOCAL, line);
    store->result = -1;
    store->a = base + 1 + a;
    store->b = args[a];
  }

  IrInst *call = csIrAppend(block, IR_CALL, line);
  call->result = -1;
  call->a = base;
  call->b = site;
  if (base + argCount + 1 > ir->slotCount) ir->slotCount = base + argCount + 1;

  /* What comes back is a number only where the callee said so. Anything else
   * is a value the compiled code may move and hand back but not do arithmetic
   * to — which is what refuses the function if it tries. */
  IrType type = csIrCallResultType(ir, site);
  int result = csIrNewRegister(ir, type);
  IrInst *load = csIrAppend(block, IR_LOAD_LOCAL, line);
  load->result = result;
  load->a = base;
  load->type = type;
  return result;
}

/* Records a call site, answering the index compiled code will use. */
int csIrAddCall(IrFunction *ir, Table *globals, ObjString *name, ObjClosure *callee, int argCount) {
  if (callee == NULL || name == NULL) return -1;

  for (int i = 0; i < ir->callCount; i++) {
    const IrCallSite *existing = &ir->calls[i];
    if (existing->callee == callee && existing->argCount == argCount && existing->globals == globals && existing->name == name) return i;
  }

  if (ir->callCount == ir->callCapacity) {
    int capacity = ir->callCapacity < 4 ? 4 : ir->callCapacity * 2;
    IrCallSite *grown = (IrCallSite *)realloc(ir->calls, sizeof(IrCallSite) * (size_t)capacity);
    if (grown == NULL) return -1;
    ir->calls = grown;
    ir->callCapacity = capacity;
  }

  IrCallSite *site = &ir->calls[ir->callCount];
  site->globals = globals;
  site->name = name;
  site->callee = callee;
  site->argCount = argCount;
  return ir->callCount++;
}
