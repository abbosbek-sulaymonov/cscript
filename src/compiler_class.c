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

    /* `class C { [key] = 1 }` — a subscript rather than a named property,
     * because the name is not known until this runs. */
    if (field->computedKey != NULL) {
      compileNode(field->computedKey);
      if (field->initializer != NULL) {
        compileNode(field->initializer);
      } else {
        emitByte(OP_UNDEFINED, fieldLine);
      }
      emitByte(OP_SET_INDEX, fieldLine);
      emitByte(OP_POP, fieldLine);
      continue;
    }

    if (field->initializer != NULL) {
      compileNode(field->initializer);
    } else {
      /* `x;` still declares the field, so every instance of the class shares
       * one layout even before anything is assigned. */
      emitByte(OP_UNDEFINED, fieldLine);
    }
    /* A private field never takes a shape slot, so it is written through the
     * private path here as well — otherwise it would be declared into the
     * layout and every read of it would look somewhere else. */
    int name = identifierConstant(field->name, field->length, fieldLine);
    if (isPrivateName(field->name, field->length)) {
      emitConstantOp(OP_SET_PRIVATE, name, fieldLine);
    } else {
      emitPropertyOp(OP_SET_PROPERTY, name, fieldLine);
    }
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
  compiler.function->paramCount = 0;
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

  /* Same rule as any other function: a parameter with a default is optional,
   * so the required count stops at the first one that has one. */
  compiler.function->paramCount = fn->as.function.paramCount;
  compiler.function->hasRest = fn->as.function.hasRest;
  compiler.function->arity = fn->as.function.paramCount;
  if (fn->as.function.hasRest) compiler.function->arity--;
  for (int i = 0; i < fn->as.function.paramCount; i++) {
    if (fn->as.function.params[i].defaultValue != NULL) {
      compiler.function->arity = i;
      break;
    }
  }
  if (fn->as.function.paramCount > UINT8_MAX) {
    errorAt(line, "too many parameters (limit %d)", UINT8_MAX);
  }
  for (int i = 0; i < fn->as.function.paramCount; i++) {
    const AstParam *param = &fn->as.function.params[i];
    addLocal(param->name, param->length, false, line);
  }
  /* Defaults and destructured parameters are dealt with before anything else,
   * including the super call — which may well want one of the names they
   * bind. */
  compileParameterPrologue(fn, line);

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
  bool isExpression = node->as.classDecl.isExpression;
  const char *name = node->as.classDecl.name;
  int nameLength = node->as.classDecl.nameLength;

  /* An anonymous class expression still needs a name to refer to itself by
   * while its body is compiled. This one is unwritable. */
  if (name == NULL) {
    name = " class";
    nameLength = 6;
  }

  emitConstantOp(OP_CLASS, identifierConstant(name, nameLength, line), line);

  /* Bound before the body is compiled, so a method can refer to the class it
   * belongs to — including to construct one.
   *
   * An expression binds it in a scope of its own instead, so the name reaches
   * the body and nothing else: `class C {}` used as a value declares no `C`
   * for the code around it, which is the whole difference between the two. */
  if (isExpression) {
    beginScope();
    addLocal(name, nameLength, true, line);
  } else if (current->scopeDepth > 0) {
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

  /* Methods first, so a static initialiser can call one. JavaScript installs
   * every method before it runs any static field or block, and a class whose
   * static block calls its own static method depends on that. */
  for (int i = 0; i < node->as.classDecl.memberCount; i++) {
    const AstClassMember *member = &node->as.classDecl.members[i];
    if (member->kind == MEMBER_STATIC_BLOCK) continue;

    /* A computed name is an expression, so it goes on the stack under the
     * closure and the installing instruction reads it from there. */
    if (member->computedKey != NULL) compileNode(member->computedKey);

    compileFunctionAs(member->function, FUNCTION_METHOD);

    uint8_t opcode = OP_METHOD;
    uint8_t computedKind = 0;
    if (member->kind == MEMBER_GETTER) {
      /* `static get x()` answers for the class rather than for an instance, so
       * it is filed apart from both the instance accessors and the statics. */
      opcode = member->isStatic ? OP_STATIC_GETTER : OP_GETTER;
      computedKind = member->isStatic ? 5 : 2;
    } else if (member->kind == MEMBER_SETTER) {
      opcode = member->isStatic ? OP_STATIC_SETTER : OP_SETTER;
      computedKind = member->isStatic ? 6 : 3;
    } else if (member->isStatic) {
      opcode = OP_STATIC_METHOD;
      computedKind = 1;
    }

    if (member->computedKey != NULL) {
      emitBytes(OP_CLASS_MEMBER, computedKind, line);
      continue;
    }

    emitConstantOp(opcode,
                   identifierConstant(member->function->as.function.name,
                                      member->function->as.function.nameLength, line),
                   line);
  }

  /* Then the static fields and the static blocks, interleaved in the order
   * they were written. A block sees the fields above it and not the ones
   * below, which is observable and is why both carry a source index. */
  int staticBlocks = 0;
  int entries = node->as.classDecl.fieldCount + node->as.classDecl.memberCount;
  for (int order = 0; order < entries; order++) {
    for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
      const AstClassField *field = &node->as.classDecl.fields[i];
      if (!field->isStatic || field->order != order) continue;
      int fieldLine = field->initializer != NULL ? field->initializer->line : line;

      if (field->computedKey != NULL) compileNode(field->computedKey);
      if (field->initializer != NULL) {
        compileNode(field->initializer);
      } else {
        emitByte(OP_UNDEFINED, fieldLine);
      }
      if (field->computedKey != NULL) {
        emitBytes(OP_CLASS_MEMBER, 4, fieldLine);
      } else {
        emitConstantOp(OP_STATIC_FIELD,
                       identifierConstant(field->name, field->length, fieldLine),
                       fieldLine);
      }
    }

    for (int i = 0; i < node->as.classDecl.memberCount; i++) {
      const AstClassMember *member = &node->as.classDecl.members[i];
      if (member->kind != MEMBER_STATIC_BLOCK || member->order != order) continue;

      /* A static block is installed like any static method and then called at
       * once, on the class the stack is still holding. Going through the
       * statics table is what gives it a receiver — and the name it is filed
       * under starts with a space, so no source can name it again. */
      compileFunctionAs(member->function, FUNCTION_METHOD);

      char hidden[24];
      int hiddenLength = snprintf(hidden, sizeof hidden, " static%d", staticBlocks++);
      int hiddenName = identifierConstant(hidden, hiddenLength, line);

      emitConstantOp(OP_STATIC_METHOD, hiddenName, line);
      emitByte(OP_DUP, line); /* the receiver the call consumes */
      emitConstantOp(OP_INVOKE, hiddenName, line);
      emitByte(0, line);
      emitByte(OP_POP, line); /* whatever the block returned */
    }
  }

  emitByte(OP_POP, line);
  if (hasSuper) endScope(line);

  /* The class is sitting in its own local's slot, which — a local being a
   * stack slot here — is already exactly where an expression leaves its value.
   * So the binding is forgotten rather than popped. */
  if (isExpression) {
    current->scopeDepth--;
    current->localCount--;
  }
}
