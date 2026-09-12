/* compiler_function.c — compiling a function, and what `this` means inside one.
 *
 * The parameter prologue, the closure, and the two loads that are not
 * variables: `this` is a frame slot a method's caller filled, and `super` is a
 * hidden local the class body captured. Both refuse outside the shape that
 * gives them meaning, which is why they answer whether they emitted anything
 * rather than emitting a guess.
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
          (uint8_t)(param->hasAnnotation ? param->type : TYPE_DYNAMIC);
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
  FunctionKind kind = FUNCTION_BODY;
  if (node->as.function.isMethod) kind = FUNCTION_METHOD;
  else if (node->as.function.isArrow) kind = FUNCTION_ARROW;
  compileFunctionAs(node, kind);
}

/* Pushes `this`, which is slot 0 of the nearest enclosing method — directly
 * when compiling that method, and through the upvalue machinery from an arrow
 * function nested inside it. */
bool compileThisLoad(int line) {
  /* Whichever function owns slot 0 has to be told, because that is the one
   * whose calls must put something there: a plain call blanks it, and `new`
   * puts the object being built in it. Arrows are skipped — they have no slot
   * 0 of their own, which is exactly why `this` reads through them. */
  for (Compiler *owner = current; owner != NULL; owner = owner->enclosing) {
    if (owner->kind == FUNCTION_ARROW) continue;
    if (owner->kind != FUNCTION_SCRIPT) owner->function->usesThis = true;
    break;
  }

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
  errorAt(line, "'this' is only valid inside a function or a method, and the "
                "top level of a module is neither");
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
