/* compiler_assign.c — assignment, in all the forms it takes.
 *
 * `x = v`, `o.x = v`, `xs[i] = v`, the compound forms, `++`/`--`, and the
 * logical ones that must not store when the operator says not to. What makes
 * these their own file is that every one of them has to read a target and then
 * write the same target — so the target is evaluated once and kept, rather
 * than compiled twice and hoped over.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler/compiler_internal.h"

/* Stores into a variable target, with the value already on the stack.
 *
 * The const check lives here so every path through assignment gets it, and
 * `discard` is set when the assignment's value is thrown away — which lets the
 * store and the pop fuse into one instruction. */
static void emitIdentifierStore(const AstNode *target, bool discard, int line) {
  const char *name = target->as.identifier.name;
  int length = target->as.identifier.length;

  int slot = resolveLocal(current, name, length);
  if (slot != -1) {
    if (current->locals[slot].isConst) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
      return;
    }
    emitBytes(discard ? OP_SET_LOCAL_POP : OP_SET_LOCAL, (uint8_t)slot, line);
    return;
  }

  int upvalue = resolveUpvalue(current, name, length, line);
  if (upvalue != -1) {
    if (enclosingLocalIsConst(current, name, length)) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
      return;
    }
    emitBytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
    if (discard) emitByte(OP_POP, line);
    return;
  }

  GlobalDecl *global = findGlobal(name, length);
  if (global != NULL && global->isConst) {
    errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
    return;
  }
  emitGlobalOp(discard ? OP_SET_GLOBAL_POP : OP_SET_GLOBAL, identifierConstant(name, length, line), line);
}

/* The jump that skips the store, for `&&=`, `||=` and `??=`.
 *
 * Each is taken when the value already there settles the question, so the
 * right side is never evaluated and no store happens — which is the whole
 * difference between `a ||= b` and `a = a || b`, and matters when the target
 * is a property with a setter or a value someone is watching. */
static uint8_t logicalSkipJump(AssignKind kind) {
  switch (kind) {
    case ASSIGN_AND: return OP_JUMP_IF_FALSE;
    case ASSIGN_OR: return OP_JUMP_IF_TRUE;
    default: return OP_JUMP_IF_NOT_NULLISH;
  }
}

/* `target op= value`, where the old value has to be read before the new one
 * is written.
 *
 * The target's own operands — the object of a property, the target and index
 * of a subscript — are evaluated *once* and duplicated. Expanding these into
 * `target = target op value` in the parser instead was what made `f().x += 1`
 * call `f` twice.
 */
static void compileReadModifyWrite(const AstNode *node, bool discard) {
  const AstNode *target = node->as.assign.target;
  AssignKind kind = node->as.assign.kind;
  int line = node->line;
  bool isLogical = kind != ASSIGN_COMPOUND;

  /* How many values sit under the old one and have to be cleared if the
   * store is skipped: the object, or the target and the index. */
  int beneath = 0;
  int nameConstant = -1;

  switch (target->type) {
    case AST_PROPERTY:
      compileNode(target->as.property.object);
      emitByte(OP_DUP, line);
      nameConstant = identifierConstant(target->as.property.name, target->as.property.length, line);
      if (isPrivateName(target->as.property.name, target->as.property.length)) {
        emitConstantOp(OP_GET_PRIVATE, nameConstant, line);
      } else {
        emitPropertyOp(OP_GET_PROPERTY, nameConstant, line);
      }
      beneath = 1;
      break;

    case AST_INDEX:
      compileNode(target->as.index.target);
      compileNode(target->as.index.index);
      emitByte(OP_DUP2, line);
      emitByte(OP_GET_INDEX, line);
      beneath = 2;
      break;

    default: compileIdentifierLoad(target->as.identifier.name, target->as.identifier.length, line); break;
  }

  int skip = isLogical ? emitJump(logicalSkipJump(kind), line) : -1;
  if (isLogical) emitByte(OP_POP, line); /* the old value; the new one replaces it */

  compileNode(node->as.assign.value);
  if (!isLogical) emitByte(binaryOpcode(node->as.assign.compoundOp), line);

  switch (target->type) {
    case AST_PROPERTY:
      if (isPrivateName(target->as.property.name, target->as.property.length)) {
        emitConstantOp(OP_SET_PRIVATE, nameConstant, line);
        if (discard) emitByte(OP_POP, line);
      } else {
        emitPropertyOp(discard ? OP_SET_PROPERTY_POP : OP_SET_PROPERTY, nameConstant, line);
      }
      break;
    case AST_INDEX:
      emitByte(OP_SET_INDEX, line);
      if (discard) emitByte(OP_POP, line);
      break;
    default: emitIdentifierStore(target, discard, line); break;
  }

  if (!isLogical) return;

  /* The short-circuit path still holds the target's operands under the old
   * value, and the two paths have to leave the stack the same height. */
  int over = emitJump(OP_JUMP, line);
  patchJump(skip, line);
  if (discard) {
    for (int i = 0; i < beneath + 1; i++) emitByte(OP_POP, line);
  } else {
    for (int i = 0; i < beneath; i++) emitByte(OP_POP_UNDER, line);
  }
  patchJump(over, line);
}

void compileAssign(const AstNode *node, bool discard) {
  const AstNode *target = node->as.assign.target;
  int assignLine = node->line;

  if (node->as.assign.kind != ASSIGN_PLAIN) {
    compileReadModifyWrite(node, discard);
    return;
  }

  if (target->type == AST_PROPERTY && isPrivateName(target->as.property.name, target->as.property.length)) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitConstantOp(OP_SET_PRIVATE, identifierConstant(target->as.property.name, target->as.property.length, assignLine), assignLine);
    if (discard) emitByte(OP_POP, assignLine);
    return;
  }

  if (target->type == AST_PROPERTY) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitPropertyOp(discard ? OP_SET_PROPERTY_POP : OP_SET_PROPERTY, identifierConstant(target->as.property.name, target->as.property.length, assignLine), assignLine);
    return;
  }

  if (target->type == AST_INDEX) {
    compileNode(target->as.index.target);
    compileNode(target->as.index.index);
    compileNode(node->as.assign.value);
    emitByte(OP_SET_INDEX, assignLine);
    if (discard) emitByte(OP_POP, assignLine);
    return;
  }

  compileNode(node->as.assign.value);
  emitIdentifierStore(target, discard, assignLine);
}

/* ++x / x++ / --x / x--
 *
 * Prefix leaves the updated value; postfix leaves the value from before the
 * update, which is why the old value is duplicated first. */
void compileUpdate(const AstNode *node) {
  const AstNode *target = node->as.update.target;
  const char *name = target->as.identifier.name;
  int length = target->as.identifier.length;
  int line = node->line;

  int slot = resolveLocal(current, name, length);
  if (slot != -1 && current->locals[slot].isConst) {
    errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
    return;
  }
  if (slot == -1) {
    GlobalDecl *global = findGlobal(name, length);
    if (global != NULL && global->isConst) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
      return;
    }
  }

  int upvalue = slot == -1 ? resolveUpvalue(current, name, length, line) : -1;
  int nameConstant = 0;
  if (slot == -1 && upvalue == -1) nameConstant = identifierConstant(name, length, line);

  compileIdentifierLoad(name, length, line);
  if (!node->as.update.isPrefix) emitByte(OP_DUP, line);

  emitConstant(NUMBER_VAL(1), line);
  emitByte(node->as.update.isIncrement ? OP_ADD : OP_SUBTRACT, line);

  if (slot != -1) {
    emitBytes(OP_SET_LOCAL, (uint8_t)slot, line);
  } else if (upvalue != -1) {
    emitBytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
  } else {
    emitGlobalOp(OP_SET_GLOBAL, nameConstant, line);
  }

  /* The store leaves the new value on top; postfix wants the old one. */
  if (!node->as.update.isPrefix) emitByte(OP_POP, line);
}

/* `yield* xs` — yield everything `xs` produces, one at a time.
 *
 * A loop rather than an opcode, because delegating means suspending once per
 * element and an instruction that suspends cannot also be the loop around
 * itself. The iterable and the position live in locals, which is why this is
 * only reachable in statement position: an expression compiles with values
 * already on the stack, and a local's slot is its height. */
static void compileYieldDelegate(const AstNode *node) {
  int line = node->line;
  beginScope();

  compileNode(node->as.yield.value);
  emitBytes(OP_ITER_PREPARE, 0, line);
  addLocal(" delegate", 9, true, line);
  int sourceSlot = current->localCount - 1;

  emitConstant(NUMBER_VAL(0), line);
  addLocal(" position", 9, false, line);
  int positionSlot = current->localCount - 1;

  int loopStart = currentChunk()->count;
  emitBytes(OP_GET_LOCAL, (uint8_t)sourceSlot, line);
  emitBytes(OP_GET_LOCAL, (uint8_t)positionSlot, line);
  int exitJump = emitJump(OP_ITER_STEP, line);

  emitByte(OP_YIELD, line);
  emitByte(OP_POP, line); /* whatever next() sent; a delegate passes it nowhere */
  emitBytes(OP_INC_LOCAL, (uint8_t)positionSlot, line);
  emitLoop(loopStart, line);

  patchJump(exitJump, line);
  endScope(line);
}

/* Compiles an expression whose value is thrown away.
 *
 * `i++` as a statement is the common case worth special-casing: the general
 * form has to produce the old value, which costs a duplicate and two pops that
 * nothing ever reads. In effect position none of that is observable, so a local
 * update collapses to a single in-place instruction. */
void compileForEffect(const AstNode *node) {
  if (node != NULL && node->type == AST_YIELD && node->as.yield.isDelegate) {
    compileYieldDelegate(node);
    return;
  }

  if (node != NULL && node->type == AST_UPDATE) {
    const AstNode *target = node->as.update.target;
    const char *name = target->as.identifier.name;
    int length = target->as.identifier.length;
    int slot = resolveLocal(current, name, length);

    if (slot != -1 && !current->locals[slot].isConst) {
      emitBytes(node->as.update.isIncrement ? OP_INC_LOCAL : OP_DEC_LOCAL, (uint8_t)slot, node->line);
      return;
    }
  }

  /* An assignment whose value is discarded fuses its store with the pop. */
  if (node != NULL && node->type == AST_ASSIGN && (node->as.assign.target->type == AST_IDENTIFIER || node->as.assign.target->type == AST_PROPERTY)) {
    compileAssign(node, true);
    return;
  }

  compileNode(node);
  emitByte(OP_POP, node != NULL ? node->line : 0);
}
