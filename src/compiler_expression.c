/* compiler_expression.c — code for things that produce a value.
 *
 * Operators, assignment, identifier loads, calls into the closure machinery,\n * and `this`/`super`. Also the two peephole decisions that live at expression\n * level: fusing a comparison into the jump that consumes it, and choosing a\n * superinstruction when the operand shapes allow one.
 */
#include <stdio.h>
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

void compileBinary(const AstNode *node) {
  compileOperandPair(node->as.binary.left, node->as.binary.right, node->line);

  int line = node->line;

  /* Where the checker resolved both sides to `number`, the generic OP_ADD's
   * string test is dead weight. This is the hook the rest of the specialisation
   * work hangs off: the types are consumed, not erased. */
  bool bothNumbers = node->as.binary.left->resolvedType == TYPE_NUMBER &&
                     node->as.binary.right->resolvedType == TYPE_NUMBER;

  switch (node->as.binary.op) {
    case BINARY_ADD:
      emitByte(bothNumbers ? OP_ADD_NUM : OP_ADD, line);
      break;
    case BINARY_SUBTRACT:      emitByte(OP_SUBTRACT, line); break;
    case BINARY_MULTIPLY:      emitByte(OP_MULTIPLY, line); break;
    case BINARY_DIVIDE:        emitByte(OP_DIVIDE, line); break;
    case BINARY_MODULO:        emitByte(OP_MODULO, line); break;
    case BINARY_EXPONENT:      emitByte(OP_EXPONENT, line); break;
    case BINARY_EQUAL:         emitByte(OP_EQUAL, line); break;
    case BINARY_NOT_EQUAL:     emitByte(OP_NOT_EQUAL, line); break;
    case BINARY_GREATER:       emitByte(OP_GREATER, line); break;
    case BINARY_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL, line); break;
    case BINARY_LESS:          emitByte(OP_LESS, line); break;
    case BINARY_LESS_EQUAL:    emitByte(OP_LESS_EQUAL, line); break;
    case BINARY_INSTANCEOF:    emitByte(OP_INSTANCEOF, line); break;
  }
}

/* && and || evaluate to an operand, not to a boolean, so they compile to a
 * conditional jump that leaves the left value on the stack. */
void compileLogical(const AstNode *node) {
  int line = node->line;
  compileNode(node->as.logical.left);

  uint8_t jumpOp =
      node->as.logical.op == LOGICAL_AND ? OP_JUMP_IF_FALSE : OP_JUMP_IF_TRUE;
  int endJump = emitJump(jumpOp, line);

  /* Not short-circuiting: drop the left value, the right one is the result. */
  emitByte(OP_POP, line);
  compileNode(node->as.logical.right);
  patchJump(endJump, line);
}

void compileIdentifierLoad(const char *name, int length, int line) {
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
void compileAssign(const AstNode *node, bool discard) {
  const AstNode *target = node->as.assign.target;
  int assignLine = node->line;

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
    return;
  }

  const char *name = target->as.identifier.name;
  int length = target->as.identifier.length;
  int line = node->line;

  int slot = resolveLocal(current, name, length);
  if (slot != -1) {
    if (current->locals[slot].isConst) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
      return;
    }
    compileNode(node->as.assign.value);
    emitBytes(discard ? OP_SET_LOCAL_POP : OP_SET_LOCAL, (uint8_t)slot, line);
    return;
  }

  int upvalue = resolveUpvalue(current, name, length, line);
  if (upvalue != -1) {
    if (enclosingLocalIsConst(current, name, length)) {
      errorAt(line, "'%.*s' is declared const and cannot be reassigned", length, name);
      return;
    }
    compileNode(node->as.assign.value);
    emitBytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
    return;
  }

  GlobalDecl *global = findGlobal(name, length);
  if (global != NULL && global->isConst) {
    errorAt(line, "'%.*s' is declared const and cannot be reassigned", length,
            name);
    return;
  }

  compileNode(node->as.assign.value);
  emitGlobalOp(discard ? OP_SET_GLOBAL_POP : OP_SET_GLOBAL, identifierConstant(name, length, line), line);
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
void compileForEffect(const AstNode *node) {
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
  beginScope();

  compiler.function->arity = node->as.function.paramCount;
  if (node->as.function.paramCount > UINT8_MAX) {
    errorAt(line, "too many parameters (limit %d)", UINT8_MAX);
  }

  /* Parameters occupy the slots directly above the callee, in order, which is
   * exactly where the caller leaves the arguments. */
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    addLocal(param->name, param->length, false, line);
  }

  /* A destructured parameter arrived under a generated name; the pattern it
   * was written as is unpacked from that slot before the body runs. */
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    if (param->pattern == NULL) continue;
    emitBytes(OP_GET_LOCAL, (uint8_t)(i + 1), line);
    compileDestructurePattern(param->pattern, line);
  }

  compileStatements(node->as.function.body->as.block.statements,
                    node->as.function.body->as.block.count);

  /* No endScope(): the whole frame is discarded by OP_RETURN, so popping the
   * locals first would be wasted work. */
  ObjFunction *function = endFunction(line);
  currentTry = enclosingTry;
  emitClosure(&compiler, function, line);
}

void compileFunction(const AstNode *node) {
  compileFunctionAs(node, FUNCTION_BODY);
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
