/* compiler_class.c — classes.
 *
 * A class is built once, where it is declared: the class object, its methods\n * and accessors, its static members, and the field initialisers that run per\n * instance. The constructor is compiled here rather than with other functions\n * because field initialisers have to land after its `super(...)` call.
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


/* How many fields belong to an instance rather than to the class. */
int instanceFieldCount(const AstNode *node) {
  int count = 0;
  for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
    if (!node->as.classDecl.fields[i].isStatic) count++;
  }
  return count;
}

void emitFieldAssignments(const AstNode *node) {
  for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
    const AstClassField *field = &node->as.classDecl.fields[i];
    /* A static field belongs to the class and is set once, where the class is
     * built — not here, which runs per instance. */
    if (field->isStatic) continue;
    int fieldLine = field->initializer != NULL ? field->initializer->line : node->line;

    emitBytes(OP_GET_LOCAL, 0, fieldLine);
    if (field->initializer != NULL) {
      compileNode(field->initializer);
    } else {
      /* `x;` still declares the field, so every instance of the class shares
       * one layout even before anything is assigned. */
      emitByte(OP_UNDEFINED, fieldLine);
    }
    emitPropertyOp(OP_SET_PROPERTY,
                   identifierConstant(field->name, field->length, fieldLine), fieldLine);
    emitByte(OP_POP, fieldLine);
  }
}

/* Only for a class that declares no constructor: its fields become a hidden
 * method the VM calls at the point the implicit constructor would have. A
 * class *with* a constructor gets them compiled into it instead. */
void compileFieldInitializer(const AstNode *node) {
  int line = node->line;

  TryContext *enclosingTry = currentTry;
  currentTry = NULL;

  Compiler compiler;
  beginFunction(&compiler, FUNCTION_METHOD, " fields", 7);
  compiler.function->arity = 0;
  emitFieldAssignments(node);

  ObjFunction *function = endFunction(line);
  currentTry = enclosingTry;
  emitClosure(&compiler, function, line);
}

/* True for the `super(...);` a subclass constructor must open with. */
bool isSuperCallStatement(const AstNode *statement) {
  return statement != NULL && statement->type == AST_EXPRESSION_STMT &&
         statement->as.expression != NULL &&
         statement->as.expression->type == AST_CALL &&
         statement->as.expression->as.call.callee != NULL &&
         statement->as.expression->as.call.callee->type == AST_SUPER &&
         statement->as.expression->as.call.callee->as.super.name == NULL;
}

/* A constructor, with the class's field initialisers spliced in where
 * JavaScript runs them: at the top of the body for a base class, and directly
 * after `super(...)` for a derived one. Anywhere else and the fields would
 * land in the wrong order relative to whatever the constructors assign, which
 * is visible through Object.keys. */
void compileConstructor(const AstNode *classNode) {
  const AstNode *fn = classNode->as.classDecl.constructor;
  const AstNode *body = fn->as.function.body;
  bool hasSuper = classNode->as.classDecl.superName != NULL;
  int line = fn->line;

  TryContext *enclosingTry = currentTry;
  currentTry = NULL;

  Compiler compiler;
  beginFunction(&compiler, FUNCTION_CONSTRUCTOR, fn->as.function.name,
                fn->as.function.nameLength);
  beginScope();

  compiler.function->arity = fn->as.function.paramCount;
  if (fn->as.function.paramCount > UINT8_MAX) {
    errorAt(line, "too many parameters (limit %d)", UINT8_MAX);
  }
  for (int i = 0; i < fn->as.function.paramCount; i++) {
    const AstParam *param = &fn->as.function.params[i];
    addLocal(param->name, param->length, false, line);
  }
  /* A destructured parameter is unpacked before anything else, including the
   * super call — which may well want one of the names it binds. */
  for (int i = 0; i < fn->as.function.paramCount; i++) {
    const AstParam *param = &fn->as.function.params[i];
    if (param->pattern == NULL) continue;
    emitBytes(OP_GET_LOCAL, (uint8_t)(i + 1), line);
    compileDestructurePattern(param->pattern, line);
  }

  int first = 0;
  if (hasSuper) {
    /* JavaScript only requires `super(...)` before the first use of `this`.
     * Requiring it first is stricter, and it is what makes the field
     * initialisers below land at a point the reader can see. */
    if (!isSuperCallStatement(body->as.block.count > 0 ? body->as.block.statements[0]
                                                       : NULL)) {
      errorAt(line, "a subclass constructor must call super(...) as its first "
                    "statement");
    } else {
      compileNode(body->as.block.statements[0]);
      first = 1;
    }
  }

  emitFieldAssignments(classNode);
  compileStatements(body->as.block.statements + first, body->as.block.count - first);

  ObjFunction *function = endFunction(line);
  currentTry = enclosingTry;
  emitClosure(&compiler, function, line);
}

void compileClassDecl(const AstNode *node) {
  int line = node->line;
  const char *name = node->as.classDecl.name;
  int nameLength = node->as.classDecl.nameLength;

  emitConstantOp(OP_CLASS, identifierConstant(name, nameLength, line), line);

  /* Bound before the body is compiled, so a method can refer to the class it
   * belongs to — including to construct one. */
  if (current->scopeDepth > 0) {
    addLocal(name, nameLength, true, line);
  } else {
    addGlobal(name, nameLength, true, line);
    emitConstantOp(OP_DEFINE_CONST, identifierConstant(name, nameLength, line), line);
  }

  bool hasSuper = node->as.classDecl.superName != NULL;
  if (hasSuper) {
    compileIdentifierLoad(node->as.classDecl.superName,
                          node->as.classDecl.superLength, line);
    /* The superclass stays on the stack as a hidden local for the whole class
     * body; OP_INHERIT reads it from there and leaves it behind. */
    beginScope();
    addLocal(" super", 6, true, line);
    compileIdentifierLoad(name, nameLength, line);
    emitByte(OP_INHERIT, line);
  }

  /* Every member opcode below expects the class on top of the stack. */
  compileIdentifierLoad(name, nameLength, line);

  if (node->as.classDecl.constructor != NULL) {
    compileConstructor(node);
    emitByte(OP_CONSTRUCTOR, line);
  } else if (instanceFieldCount(node) > 0) {
    compileFieldInitializer(node);
    emitByte(OP_FIELD_INIT, line);
  }

  /* Static fields are set on the class itself, while it is still on the stack.
   * They are ordinary values rather than per-instance work. */
  for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
    const AstClassField *field = &node->as.classDecl.fields[i];
    if (!field->isStatic) continue;
    int fieldLine = field->initializer != NULL ? field->initializer->line : line;

    if (field->initializer != NULL) {
      compileNode(field->initializer);
    } else {
      emitByte(OP_UNDEFINED, fieldLine);
    }
    emitConstantOp(OP_STATIC_FIELD,
                   identifierConstant(field->name, field->length, fieldLine),
                   fieldLine);
  }

  for (int i = 0; i < node->as.classDecl.memberCount; i++) {
    const AstClassMember *member = &node->as.classDecl.members[i];
    compileFunctionAs(member->function, FUNCTION_METHOD);

    uint8_t opcode = OP_METHOD;
    if (member->kind == MEMBER_GETTER) {
      opcode = OP_GETTER;
    } else if (member->kind == MEMBER_SETTER) {
      opcode = OP_SETTER;
    } else if (member->isStatic) {
      opcode = OP_STATIC_METHOD;
    }

    emitConstantOp(opcode,
                   identifierConstant(member->function->as.function.name,
                                      member->function->as.function.nameLength, line),
                   line);
  }

  emitByte(OP_POP, line);
  if (hasSuper) endScope(line);
}
