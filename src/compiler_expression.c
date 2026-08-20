/* compiler_expression.c — code for things that produce a value.
 *
 * Operators, assignment, identifier loads, calls into the closure machinery,\n * and `this`/`super`. Also the two peephole decisions that live at expression\n * level: fusing a comparison into the jump that consumes it, and choosing a\n * superinstruction when the operand shapes allow one.
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
#include "compiler_internal.h"


/* Emits a binary operator's two operands, fusing "a local, then a literal" into
 * one instruction. That shape covers `i < n`, `i % 7` and `total + 1`, and
 * profiling put it at 14-18% of everything executed in loop-heavy code. */
void compileOperandPair(const AstNode *left, const AstNode *right, int line) {
  if (left->type == AST_IDENTIFIER && right->type == AST_NUMBER_LITERAL) {
    int slot = resolveLocal(current, left->as.identifier.name,
                            left->as.identifier.length);
    if (slot != -1) {
      emitByte(OP_GET_LOCAL_CONST, line);
      emitByte((uint8_t)slot, line);
      emitConstantOperand(makeConstant(NUMBER_VAL(right->as.number), line), line);
      return;
    }
  }

  if (left->type == AST_IDENTIFIER && right->type == AST_IDENTIFIER) {
    int leftSlot = resolveLocal(current, left->as.identifier.name,
                                left->as.identifier.length);
    int rightSlot = resolveLocal(current, right->as.identifier.name,
                                 right->as.identifier.length);
    if (leftSlot != -1 && rightSlot != -1) {
      emitByte(OP_GET_LOCAL_LOCAL, line);
      emitByte((uint8_t)leftSlot, line);
      emitByte((uint8_t)rightSlot, line);
      return;
    }
  }

  compileNode(left);
  compileNode(right);
}

/* The instruction a binary operator compiles to, ignoring specialisation.
 * Shared with compound assignment, which builds the same operation without an
 * AST_BINARY node to hang resolved types off. */
uint8_t binaryOpcode(BinaryOp op) {
  switch (op) {
    case BINARY_ADD:           return OP_ADD;
    case BINARY_SUBTRACT:      return OP_SUBTRACT;
    case BINARY_MULTIPLY:      return OP_MULTIPLY;
    case BINARY_DIVIDE:        return OP_DIVIDE;
    case BINARY_MODULO:        return OP_MODULO;
    case BINARY_EXPONENT:      return OP_EXPONENT;
    case BINARY_EQUAL:         return OP_EQUAL;
    case BINARY_NOT_EQUAL:     return OP_NOT_EQUAL;
    case BINARY_GREATER:       return OP_GREATER;
    case BINARY_GREATER_EQUAL: return OP_GREATER_EQUAL;
    case BINARY_LESS:          return OP_LESS;
    case BINARY_LESS_EQUAL:    return OP_LESS_EQUAL;
    case BINARY_INSTANCEOF:    return OP_INSTANCEOF;
    case BINARY_IN:            return OP_IN;
  }
  return OP_ADD;
}

void compileBinary(const AstNode *node) {
  compileOperandPair(node->as.binary.left, node->as.binary.right, node->line);

  int line = node->line;

  /* Where the checker resolved both sides to `number`, the generic OP_ADD's
   * string test is dead weight. This is the hook the rest of the specialisation
   * work hangs off: the types are consumed, not erased. */
  bool bothNumbers = node->as.binary.left->resolvedType == TYPE_NUMBER &&
                     node->as.binary.right->resolvedType == TYPE_NUMBER;

  /* Recorded for tiering: an operation whose operand types are known is one a
   * compiler could emit unboxed, with no guard and nothing to deoptimise to.
   * The ratio across a hot function is what decides whether a type-directed
   * backend is worth building — see jit.h. */
  if (bothNumbers) {
    current->function->typedSites++;
  } else {
    current->function->genericSites++;
  }

  emitByte(node->as.binary.op == BINARY_ADD && bothNumbers
               ? OP_ADD_NUM
               : binaryOpcode(node->as.binary.op),
           line);
}

/* Optional chaining.
 *
 * `a?.b.c()` short-circuits the *chain*, not the link: a nullish `a` skips the
 * `.c` and the call as well. So every `?.` in one chain jumps to the same
 * place, and only the outermost expression knows where that is — which is why
 * the parser wraps the chain in a node of its own.
 *
 * Chains nest only as expressions nest (`a?.b(c?.d)`), so a small fixed stack
 * is enough; nothing a program can grow feeds it. */
#define CS_MAX_CHAIN_DEPTH 16
#define CS_MAX_CHAIN_LINKS 32

typedef struct {
  int jumps[CS_MAX_CHAIN_LINKS];
  int count;
} OptionalChain;

static OptionalChain chains[CS_MAX_CHAIN_DEPTH];
static int chainDepth = 0;

/* Emitted with the value that `?.` tests on top of the stack, and nothing of
 * the chain's below it — which is what lets the landing site replace exactly
 * one value regardless of which link jumped. */
void emitOptionalGuard(int line) {
  if (chainDepth == 0) return;
  OptionalChain *chain = &chains[chainDepth - 1];
  if (chain->count >= CS_MAX_CHAIN_LINKS) {
    errorAt(line, "too many '?.' links in one chain (limit %d)", CS_MAX_CHAIN_LINKS);
    return;
  }
  chain->jumps[chain->count++] = emitJump(OP_JUMP_IF_NULLISH, line);
}

/* An unconditional jump into the same landing site. Used where a link has to
 * tidy the stack before it short-circuits. */
void emitOptionalJump(int line) {
  if (chainDepth == 0) return;
  OptionalChain *chain = &chains[chainDepth - 1];
  if (chain->count >= CS_MAX_CHAIN_LINKS) {
    errorAt(line, "too many '?.' links in one chain (limit %d)", CS_MAX_CHAIN_LINKS);
    return;
  }
  chain->jumps[chain->count++] = emitJump(OP_JUMP, line);
}

void compileOptionalChain(const AstNode *node) {
  int line = node->line;
  if (chainDepth >= CS_MAX_CHAIN_DEPTH) {
    errorAt(line, "'?.' chains nested too deeply (limit %d)", CS_MAX_CHAIN_DEPTH);
    return;
  }

  chains[chainDepth++].count = 0;
  compileNode(node->as.expression);
  OptionalChain *chain = &chains[--chainDepth];
  if (chain->count == 0) return;

  int over = emitJump(OP_JUMP, line);
  for (int i = 0; i < chain->count; i++) patchJump(chain->jumps[i], line);
  /* The result of a short-circuit is undefined even when the value that
   * caused it was null: `null?.x` is undefined, which is observable. */
  emitByte(OP_POP, line);
  emitByte(OP_UNDEFINED, line);
  patchJump(over, line);
}

/* && and || evaluate to an operand, not to a boolean, so they compile to a
 * conditional jump that leaves the left value on the stack. */
/* OP_AWAIT and the note that this body needs a fiber to run on.
 *
 * The two always go together: `await` suspends a fiber, and the top level of a
 * file only gets one when it is marked. Emitting the instruction anywhere
 * without the mark produces a body that compiles and cannot run. */
void emitAwait(int line) {
  emitByte(OP_AWAIT, line);
  if (current->kind == FUNCTION_SCRIPT) current->function->isAsync = true;
}

void compileLogical(const AstNode *node) {
  int line = node->line;
  compileNode(node->as.logical.left);

  /* `??` asks a different question from `||`: whether the left side is
   * *present*, not whether it is truthy. `0 ?? 1` is 0. */
  uint8_t jumpOp = node->as.logical.op == LOGICAL_AND    ? OP_JUMP_IF_FALSE
                   : node->as.logical.op == LOGICAL_OR   ? OP_JUMP_IF_TRUE
                                                         : OP_JUMP_IF_NOT_NULLISH;
  int endJump = emitJump(jumpOp, line);

  /* Not short-circuiting: drop the left value, the right one is the result. */
  emitByte(OP_POP, line);
  compileNode(node->as.logical.right);
  patchJump(endJump, line);
}

void compileIdentifierLoad(const char *name, int length, int line) {
  if (isPrivateName(name, length)) {
    errorAt(line, "'%.*s' is a private name, which is only valid as a property "
                  "inside the class that declares it", length, name);
    return;
  }

  int slot = resolveLocal(current, name, length);
  if (slot != -1) {
    emitBytes(OP_GET_LOCAL, (uint8_t)slot, line);
    return;
  }

  int upvalue = resolveUpvalue(current, name, length, line);
  if (upvalue != -1) {
    emitBytes(OP_GET_UPVALUE, (uint8_t)upvalue, line);
    return;
  }

  emitGlobalOp(OP_GET_GLOBAL, identifierConstant(name, length, line), line);
}

/* `discard` is set when the assignment's value is thrown away, which lets the
 * store and the pop fuse into one instruction. */
/* Stores into a variable target, with the value already on the stack. The
 * const check lives here so every path through assignment gets it. */
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
  emitGlobalOp(discard ? OP_SET_GLOBAL_POP : OP_SET_GLOBAL,
               identifierConstant(name, length, line), line);
}

/* The jump that skips the store, for `&&=`, `||=` and `??=`.
 *
 * Each is taken when the value already there settles the question, so the
 * right side is never evaluated and no store happens — which is the whole
 * difference between `a ||= b` and `a = a || b`, and matters when the target
 * is a property with a setter or a value someone is watching. */
static uint8_t logicalSkipJump(AssignKind kind) {
  switch (kind) {
    case ASSIGN_AND:     return OP_JUMP_IF_FALSE;
    case ASSIGN_OR:      return OP_JUMP_IF_TRUE;
    default:             return OP_JUMP_IF_NOT_NULLISH;
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
      nameConstant = identifierConstant(target->as.property.name,
                                        target->as.property.length, line);
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

    default:
      compileIdentifierLoad(target->as.identifier.name,
                            target->as.identifier.length, line);
      break;
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
        emitPropertyOp(discard ? OP_SET_PROPERTY_POP : OP_SET_PROPERTY,
                       nameConstant, line);
      }
      break;
    case AST_INDEX:
      emitByte(OP_SET_INDEX, line);
      if (discard) emitByte(OP_POP, line);
      break;
    default:
      emitIdentifierStore(target, discard, line);
      break;
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

  if (target->type == AST_PROPERTY &&
      isPrivateName(target->as.property.name, target->as.property.length)) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitConstantOp(OP_SET_PRIVATE,
                   identifierConstant(target->as.property.name,
                                      target->as.property.length, assignLine),
                   assignLine);
    if (discard) emitByte(OP_POP, assignLine);
    return;
  }

  if (target->type == AST_PROPERTY) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitPropertyOp(discard ? OP_SET_PROPERTY_POP : OP_SET_PROPERTY,
                   identifierConstant(target->as.property.name,
                                      target->as.property.length, assignLine),
                   assignLine);
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
    errorAt(line, "'%.*s' is declared const and cannot be reassigned", length,
            name);
    return;
  }
  if (slot == -1) {
    GlobalDecl *global = findGlobal(name, length);
    if (global != NULL && global->isConst) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned",
              length, name);
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

/* Compiles an expression whose value is thrown away.
 *
 * `i++` as a statement is the common case worth special-casing: the general
 * form has to produce the old value, which costs a duplicate and two pops that
 * nothing ever reads. In effect position none of that is observable, so a local
 * update collapses to a single in-place instruction. */
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
  emitByte(OP_ITER_PREPARE, line);
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
      emitBytes(node->as.update.isIncrement ? OP_INC_LOCAL : OP_DEC_LOCAL,
                (uint8_t)slot, node->line);
      return;
    }
  }

  /* An assignment whose value is discarded fuses its store with the pop. */
  if (node != NULL && node->type == AST_ASSIGN &&
      (node->as.assign.target->type == AST_IDENTIFIER ||
       node->as.assign.target->type == AST_PROPERTY)) {
    compileAssign(node, true);
    return;
  }

  compileNode(node);
  emitByte(OP_POP, node != NULL ? node->line : 0);
}

/* Maps a comparison to the fused jump that tests it directly. The jump is
 * taken when the comparison is false, so each opcode is its negation. */
bool fusedConditionJump(const AstNode *condition, uint8_t *opcode) {
  if (condition == NULL || condition->type != AST_BINARY) return false;

  switch (condition->as.binary.op) {
    case BINARY_LESS:          *opcode = OP_JUMP_IF_NOT_LESS; return true;
    case BINARY_LESS_EQUAL:    *opcode = OP_JUMP_IF_NOT_LESS_EQUAL; return true;
    case BINARY_GREATER:       *opcode = OP_JUMP_IF_NOT_GREATER; return true;
    case BINARY_GREATER_EQUAL: *opcode = OP_JUMP_IF_NOT_GREATER_EQUAL; return true;
    case BINARY_EQUAL:         *opcode = OP_JUMP_IF_NOT_EQUAL; return true;
    case BINARY_NOT_EQUAL:     *opcode = OP_JUMP_IF_EQUAL; return true;
    default:                   return false;
  }
}

/* Emits a condition and the jump that skips the branch when it is false,
 * returning the offset to patch. A comparison compiles to a single fused
 * instruction rather than producing a boolean for the next one to consume. */
int emitConditionJump(const AstNode *condition, int line) {
  uint8_t fused;
  if (fusedConditionJump(condition, &fused)) {
    compileOperandPair(condition->as.binary.left, condition->as.binary.right, line);
    return emitJump(fused, line);
  }

  compileNode(condition);
  return emitJump(OP_POP_JUMP_IF_FALSE, line);
}

/* True when a subtree contains a function, which is the only way a program can
 * observe whether a loop variable is one binding or one per iteration.
 *
 * Used to decide whether a `for` loop needs the per-iteration copy below. A
 * loop with no closures in it keeps the single-slot form and stays fast. */
bool containsFunction(const AstNode *node) {
  if (node == NULL) return false;

  switch (node->type) {
    case AST_FUNCTION:
      return true;

    case AST_UNARY:    return containsFunction(node->as.unary.operand);
    case AST_GROUPING: return containsFunction(node->as.grouping);
    case AST_BINARY:
      return containsFunction(node->as.binary.left) ||
             containsFunction(node->as.binary.right);
    case AST_LOGICAL:
      return containsFunction(node->as.logical.left) ||
             containsFunction(node->as.logical.right);
    case AST_ASSIGN:
      return containsFunction(node->as.assign.target) ||
             containsFunction(node->as.assign.value);
    case AST_UPDATE:   return containsFunction(node->as.update.target);
    case AST_PROPERTY: return containsFunction(node->as.property.object);
    case AST_INDEX:
      return containsFunction(node->as.index.target) ||
             containsFunction(node->as.index.index);
    case AST_CONDITIONAL:
      return containsFunction(node->as.conditional.condition) ||
             containsFunction(node->as.conditional.thenValue) ||
             containsFunction(node->as.conditional.elseValue);

    case AST_CALL: {
      if (containsFunction(node->as.call.callee)) return true;
      for (int i = 0; i < node->as.call.argCount; i++) {
        if (containsFunction(node->as.call.arguments[i])) return true;
      }
      return false;
    }
    case AST_ARRAY_LITERAL: {
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        if (containsFunction(node->as.arrayLiteral.elements[i])) return true;
      }
      return false;
    }
    case AST_OBJECT_LITERAL: {
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        if (containsFunction(node->as.objectLiteral.values[i])) return true;
      }
      return false;
    }

    case AST_EXPRESSION_STMT: return containsFunction(node->as.expression);
    case AST_RETURN_STMT:     return containsFunction(node->as.returnValue);
    case AST_VAR_DECL:        return containsFunction(node->as.varDecl.initializer);
    case AST_IF_STMT:
      return containsFunction(node->as.ifStmt.condition) ||
             containsFunction(node->as.ifStmt.thenBranch) ||
             containsFunction(node->as.ifStmt.elseBranch);
    case AST_WHILE_STMT:
      return containsFunction(node->as.whileStmt.condition) ||
             containsFunction(node->as.whileStmt.body);
    case AST_FOR_STMT:
      return containsFunction(node->as.forStmt.initializer) ||
             containsFunction(node->as.forStmt.condition) ||
             containsFunction(node->as.forStmt.increment) ||
             containsFunction(node->as.forStmt.body);
    case AST_FOR_OF_STMT:
      return containsFunction(node->as.forOf.iterable) ||
             containsFunction(node->as.forOf.body);
    case AST_SWITCH_STMT: {
      if (containsFunction(node->as.switchStmt.subject)) return true;
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        if (containsFunction(node->as.switchStmt.cases[i].body)) return true;
      }
      return containsFunction(node->as.switchStmt.defaultBody);
    }
    case AST_BLOCK: {
      for (int i = 0; i < node->as.block.count; i++) {
        if (containsFunction(node->as.block.statements[i])) return true;
      }
      return false;
    }
    case AST_PROGRAM: {
      for (int i = 0; i < node->as.program.count; i++) {
        if (containsFunction(node->as.program.statements[i])) return true;
      }
      return false;
    }

    default:
      return false;
  }
}

/* The two things that happen to parameters before a body runs: a default
 * fills in for an argument that was not given, and a pattern is unpacked from
 * the slot the generated name holds. Shared with the constructor, which has a
 * body of its own to build and would otherwise have to remember both. */
void compileParameterPrologue(const AstNode *node, int line) {
  /* The caller padded the frame with undefined for every argument it did not
   * supply, and an argument written as `undefined` is not distinguishable from
   * one left out — which is what JavaScript says too. */
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    if (param->defaultValue == NULL) continue;

    emitBytes(OP_GET_LOCAL, (uint8_t)(i + 1), line);
    emitByte(OP_UNDEFINED, line);
    emitByte(OP_NOT_EQUAL, line);
    int given = emitJump(OP_POP_JUMP_IF_FALSE, line);
    int done = emitJump(OP_JUMP, line);

    patchJump(given, line);
    compileNode(param->defaultValue);
    emitBytes(OP_SET_LOCAL_POP, (uint8_t)(i + 1), line);
    patchJump(done, line);
  }

  /* A destructured parameter arrived under a generated name; the pattern it
   * was written as is unpacked from that slot. */
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    if (param->pattern == NULL) continue;
    emitBytes(OP_GET_LOCAL, (uint8_t)(i + 1), line);
    compileDestructurePattern(param->pattern, line);
  }
}

void compileFunctionAs(const AstNode *node, FunctionKind kind) {
  int line = node->line;

  /* A nested function's `return` must not emit the enclosing function's
   * finally blocks — they belong to a frame it will never unwind. */
  TryContext *enclosingTry = currentTry;
  currentTry = NULL;

  Compiler compiler;
  beginFunction(&compiler, kind, node->as.function.name,
                node->as.function.nameLength);
  compiler.function->isAsync = node->as.function.isAsync;
  compiler.function->isGenerator = node->as.function.isGenerator;
  beginScope();

  /* A parameter with a default is optional, so the required count stops at
   * the first one that has it — which is also why JavaScript will not let a
   * required parameter follow an optional one. */
  compiler.function->paramCount = node->as.function.paramCount;
  compiler.function->hasRest = node->as.function.hasRest;
  compiler.function->arity = node->as.function.paramCount;
  /* A rest parameter is never required: it is an empty array when nothing is
   * left over. */
  if (node->as.function.hasRest) compiler.function->arity--;
  for (int i = 0; i < node->as.function.paramCount; i++) {
    if (node->as.function.params[i].defaultValue != NULL) {
      compiler.function->arity = i;
      break;
    }
  }
  if (node->as.function.paramCount > UINT8_MAX) {
    errorAt(line, "too many parameters (limit %d)", UINT8_MAX);
  }

  /* Parameters occupy the slots directly above the callee, in order, which is
   * exactly where the caller leaves the arguments. */
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    addLocal(param->name, param->length, false, line);
  }

  /* The declared types are carried through to the run time, where the lowering
   * needs them: an annotation that stops at the compiler cannot tell a code
   * generator that an argument is a number. */
  if (node->as.function.paramCount > 0) {
    compiler.function->paramTypes =
        (uint8_t *)malloc((size_t)node->as.function.paramCount);
    for (int i = 0; i < node->as.function.paramCount; i++) {
      const AstParam *param = &node->as.function.params[i];
      compiler.function->paramTypes[i] =
          (uint8_t)(param->hasAnnotation ? param->type : TYPE_ANY);
    }
  }

  compileParameterPrologue(node, line);

  compileStatements(node->as.function.body->as.block.statements,
                    node->as.function.body->as.block.count);

  /* No endScope(): the whole frame is discarded by OP_RETURN, so popping the
   * locals first would be wasted work. */
  ObjFunction *function = endFunction(line);
  currentTry = enclosingTry;
  emitClosure(&compiler, function, line);
}

void compileFunction(const AstNode *node) {
  compileFunctionAs(node, node->as.function.isMethod ? FUNCTION_METHOD : FUNCTION_BODY);
}

/* Pushes `this`, which is slot 0 of the nearest enclosing method — directly
 * when compiling that method, and through the upvalue machinery from an arrow
 * function nested inside it. */
bool compileThisLoad(int line) {
  int slot = resolveLocal(current, "this", 4);
  if (slot != -1) {
    emitBytes(OP_GET_LOCAL, (uint8_t)slot, line);
    return true;
  }
  int upvalue = resolveUpvalue(current, "this", 4, line);
  if (upvalue != -1) {
    emitBytes(OP_GET_UPVALUE, (uint8_t)upvalue, line);
    return true;
  }
  errorAt(line, "'this' is only valid inside a class method");
  return false;
}

/* Pushes the hidden local holding the superclass. It is declared in a scope
 * wrapping the class body, so a method that mentions `super` captures it — and
 * therefore resolves it against the class the method was *written* in rather
 * than the class of the receiver, which is what makes `super.m()` from a
 * two-deep hierarchy call the right method. */
bool compileSuperLoad(int line) {
  int slot = resolveLocal(current, " super", 6);
  if (slot != -1) {
    emitBytes(OP_GET_LOCAL, (uint8_t)slot, line);
    return true;
  }
  int upvalue = resolveUpvalue(current, " super", 6, line);
  if (upvalue != -1) {
    emitBytes(OP_GET_UPVALUE, (uint8_t)upvalue, line);
    return true;
  }
  errorAt(line, "'super' is only valid inside a class that has a superclass");
  return false;
}
