#include <stdarg.h>
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

/* The compiler's ambient state. Declared in compiler_internal.h; defined here
 * so there is exactly one of each. */
Compiler *current = NULL;
Unit *currentUnit = NULL;
Loop *currentLoop = NULL;
TryContext *currentTry = NULL;

Chunk *currentChunk(void) { return &current->function->chunk; }

/* Every function still being compiled is a root: interning a string constant
 * allocates, and the constants already written must survive that. Nested
 * functions mean walking the whole chain, not just the innermost. */
void csCompilerMarkRoots(void) {
  for (Compiler *compiler = current; compiler != NULL;
       compiler = compiler->enclosing) {
    csMarkObject((Obj *)compiler->function);
  }
}

void errorAt(int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  /* csDiagnosticError takes the varargs itself, so forward through a buffer. */
  char message[256];
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  currentUnit->diag->panicMode = false;
  csDiagnosticError(currentUnit->diag, line, NULL, 0, "%s", message);
}

void emitByte(uint8_t byte, int line) {
  csChunkWrite(currentChunk(), byte, line);
}

void emitBytes(uint8_t a, uint8_t b, int line) {
  emitByte(a, line);
  emitByte(b, line);
}

/* Constant-pool indices are 16-bit, big-endian, matching the jump offsets. */
void emitConstantOperand(int index, int line) {
  emitByte((uint8_t)((index >> 8) & 0xff), line);
  emitByte((uint8_t)(index & 0xff), line);
}

/* Emits an opcode whose single operand is a constant index. */
void emitConstantOp(uint8_t opcode, int index, int line) {
  emitByte(opcode, line);
  emitConstantOperand(index, line);
}

/* Opcodes that carry an inline cache take a second 16-bit operand: the index
 * of this site's entry in the chunk's cache array. Every site gets its own, so
 * one `o.x` in a loop never fights with a different `o.x` elsewhere. */
void emitPropertyOp(uint8_t opcode, int index, int line) {
  emitConstantOp(opcode, index, line);
  int cache = csChunkAddPropertyCache(currentChunk());
  if (cache > UINT16_MAX) errorAt(line, "too many property sites in one function");
  emitConstantOperand(cache, line);
}

void emitGlobalOp(uint8_t opcode, int index, int line) {
  emitConstantOp(opcode, index, line);
  int cache = csChunkAddGlobalCache(currentChunk());
  if (cache > UINT16_MAX) errorAt(line, "too many global sites in one function");
  emitConstantOperand(cache, line);
}

/* Adds a value to the constant pool and returns its index, reusing an existing
 * entry when one matches. Identifier names repeat constantly, so deduplicating
 * keeps the pool inside the one-byte operand limit for far longer. */
int makeConstant(Value value, int line) {
  ValueArray *constants = &currentChunk()->constants;
  for (int i = 0; i < constants->count; i++) {
    if (csValuesStrictEqual(constants->values[i], value)) return i;
  }

  int index = csChunkAddConstant(currentChunk(), value);
  if (index > UINT16_MAX) {
    errorAt(line, "too many constants in one function (limit %d)", UINT16_MAX + 1);
    return 0;
  }
  return index;
}

void emitConstant(Value value, int line) {
  emitConstantOp(OP_CONSTANT, makeConstant(value, line), line);
}

/* Interns an identifier and returns its constant-pool index. */
int identifierConstant(const char *name, int length, int line) {
  ObjString *string = csStringCopy(name, length);
  return makeConstant(OBJ_VAL(string), line);
}

/* Writes a jump with a placeholder operand and returns the offset to patch. */
/* The two-byte placeholder alone, for a jump whose opcode and other operands
 * are already written — OP_JUMP_IF_NO_METHOD carries a constant first. */
int emitJump16(int line) {
  emitByte(0xff, line);
  emitByte(0xff, line);
  return currentChunk()->count - 2;
}

int emitJump(uint8_t instruction, int line) {
  emitByte(instruction, line);
  return emitJump16(line);
}

/* Fills in a jump emitted earlier, now that the target is known. */
void patchJump(int offset, int line) {
  int jump = currentChunk()->count - offset - 2;
  if (jump > UINT16_MAX) {
    errorAt(line, "jump distance exceeds %d bytes", UINT16_MAX);
    return;
  }
  currentChunk()->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  currentChunk()->code[offset + 1] = (uint8_t)(jump & 0xff);
}

void emitLoop(int loopStart, int line) {
  emitByte(OP_LOOP, line);
  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) {
    errorAt(line, "loop body is too large to jump back over");
    return;
  }
  emitByte((uint8_t)((offset >> 8) & 0xff), line);
  emitByte((uint8_t)(offset & 0xff), line);
}

/* ---------------- scope handling ---------------- */

void beginScope(void) { current->scopeDepth++; }

void endScope(int line) {
  current->scopeDepth--;

  /* Locals that were captured cannot simply be popped: a closure may outlive
   * this scope and still refer to them, so those are moved onto the heap
   * individually. Everything else is discarded in one instruction. */
  int pending = 0;
  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    Local *local = &current->locals[current->localCount - 1];

    if (local->isCaptured) {
      if (pending == 1) {
        emitByte(OP_POP, line);
      } else if (pending > 1) {
        emitBytes(OP_POP_N, (uint8_t)pending, line);
      }
      pending = 0;
      emitByte(OP_CLOSE_UPVALUE, line);
    } else {
      pending++;
    }
    current->localCount--;
  }

  if (pending == 1) {
    emitByte(OP_POP, line);
  } else if (pending > 1) {
    emitBytes(OP_POP_N, (uint8_t)pending, line);
  }
}

bool identifiersEqual(const Local *local, const char *name, int length) {
  return local->length == length && memcmp(local->name, name, (size_t)length) == 0;
}

/* Returns the stack slot for a local, or -1 when the name is not local. */
int resolveLocal(Compiler *compiler, const char *name, int length) {
  /* Search backwards so an inner declaration shadows an outer one. */
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    if (identifiersEqual(&compiler->locals[i], name, length)) return i;
  }
  return -1;
}

/* Records that `compiler` captures a variable, reusing the slot if it already
 * captured the same one. */
int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal, int line) {
  int count = compiler->function->upvalueCount;

  for (int i = 0; i < count; i++) {
    Upvalue *existing = &compiler->upvalues[i];
    if (existing->index == index && existing->isLocal == isLocal) return i;
  }

  if (count >= MAX_LOCALS) {
    errorAt(line, "too many captured variables in one function (limit %d)", MAX_LOCALS);
    return 0;
  }

  compiler->upvalues[count].isLocal = isLocal;
  compiler->upvalues[count].index = index;
  return compiler->function->upvalueCount++;
}

/* Resolves a name to an upvalue, capturing it through however many enclosing
 * functions lie between here and the declaration.
 *
 * The recursion is what makes deep capture work: if the name is a local of the
 * immediately enclosing function it is captured directly, and otherwise that
 * function is asked to capture it first, so each level in the chain ends up
 * holding an upvalue pointing at the one above it. Returns -1 when the name is
 * not a local of any enclosing function, which means it is global. */
int resolveUpvalue(Compiler *compiler, const char *name, int length, int line) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name, length);
  if (local != -1) {
    /* Mark it, so leaving that scope emits OP_CLOSE_UPVALUE instead of a plain
     * pop. Without this the upvalue is left pointing at a reused stack slot —
     * which only shows up for a captured *block* local, because a captured
     * parameter happens to be closed anyway when OP_RETURN discards the frame. */
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true, line);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name, length, line);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false, line);
  }

  return -1;
}

/* True when the named local of an enclosing function is const. */
bool enclosingLocalIsConst(Compiler *compiler, const char *name, int length) {
  for (Compiler *scope = compiler->enclosing; scope != NULL; scope = scope->enclosing) {
    int slot = resolveLocal(scope, name, length);
    if (slot != -1) return scope->locals[slot].isConst;
  }
  return false;
}

void addLocal(const char *name, int length, bool isConst,
                     int line) {
  if (current->localCount >= MAX_LOCALS) {
    errorAt(line, "too many local variables in scope (limit %d)", MAX_LOCALS);
    return;
  }

  /* Compiler-generated locals are named with a leading space, which no source
   * can produce. They are only ever referenced by slot, so two of them in one
   * scope is normal — two destructuring declarations in the same block, say —
   * and the redeclaration check does not apply. */
  bool isInternal = length > 0 && name[0] == ' ';

  /* Redeclaring a name in the same scope is a mistake, not shadowing. */
  for (int i = current->localCount - 1; !isInternal && i >= 0; i--) {
    Local *local = &current->locals[i];
    if (local->depth < current->scopeDepth) break;
    if (identifiersEqual(local, name, length)) {
      errorAt(line, "'%.*s' is already declared in this scope", length, name);
      return;
    }
  }

  Local *local = &current->locals[current->localCount++];
  local->name = name;
  local->length = length;
  local->depth = current->scopeDepth;
  local->isConst = isConst;
  local->isCaptured = false;
}

/* Returns the declaration for a global this unit declared, or NULL. */
GlobalDecl *findGlobal(const char *name, int length) {
  for (int i = 0; i < currentUnit->globalCount; i++) {
    GlobalDecl *global = &currentUnit->globals[i];
    if (global->length == length && memcmp(global->name, name, (size_t)length) == 0) {
      return global;
    }
  }
  return NULL;
}

void addGlobal(const char *name, int length, bool isConst,
                      int line) {
  if (findGlobal(name, length) != NULL) {
    errorAt(line, "'%.*s' is already declared", length, name);
    return;
  }
  if (currentUnit->globalCount >= MAX_GLOBALS) {
    errorAt(line, "too many global variables (limit %d)", MAX_GLOBALS);
    return;
  }

  GlobalDecl *global = &currentUnit->globals[currentUnit->globalCount++];
  global->name = name;
  global->length = length;
  global->isConst = isConst;
}


const char *pendingLabel = NULL;
int pendingLabelLength = 0;

/* The loop or switch a `break` / `continue` names.
 *
 * Without a label that is the innermost one that accepts the jump; with one it
 * is the nearest enclosing construct carrying that label, which is how
 * `break outer` leaves more than one loop at once. */
static Loop *targetLoop(const char *label, int labelLength, bool needsContinue,
                        int line) {
  for (Loop *loop = currentLoop; loop != NULL; loop = loop->enclosing) {
    if (label == NULL) {
      if (!needsContinue || loop->allowsContinue) return loop;
      continue;
    }
    if (loop->label == NULL || loop->labelLength != labelLength ||
        memcmp(loop->label, label, (size_t)labelLength) != 0) {
      continue;
    }
    if (needsContinue && !loop->allowsContinue) {
      errorAt(line, "'continue %.*s' names a label that is not on a loop",
              labelLength, label);
      return NULL;
    }
    return loop;
  }

  if (label != NULL) {
    errorAt(line, "no enclosing statement is labelled '%.*s'", labelLength, label);
  } else if (needsContinue) {
    errorAt(line, "'continue' outside of a loop");
  } else {
    errorAt(line, "'break' outside of a loop or switch");
  }
  return NULL;
}

void beginLoop(Loop *loop, bool allowsContinue) {
  loop->enclosing = currentLoop;
  /* Taken rather than copied: the label belongs to the statement immediately
   * after it, and a loop nested inside that one must not inherit it. */
  loop->label = pendingLabel;
  loop->labelLength = pendingLabelLength;
  pendingLabel = NULL;
  pendingLabelLength = 0;
  loop->scopeDepth = current->scopeDepth;
  loop->continueTarget = -1;
  loop->allowsContinue = allowsContinue;
  loop->breakCount = 0;
  loop->continueCount = 0;
  currentLoop = loop;
}

void endLoop(Loop *loop, int line) {
  for (int i = 0; i < loop->breakCount; i++) {
    patchJump(loop->breakJumps[i], line);
  }
  currentLoop = loop->enclosing;
}

/* Emits the finally blocks for every open try down to `stopAtDepth`, closing
 * each handler on the way. Passing -1 means "all of them in this function",
 * which is what a `return` needs; a `break` stops at the loop's own depth. */
void compileNode(const AstNode *node);


/* ---------------- code generation ---------------- */

void compileNode(const AstNode *node);








/* `const [a, b] = xs;` and `const { x, y } = o;`
 *
 * Compiles to the loads and stores the pattern stands for, so nothing new
 * exists at run time. The source is evaluated once into a hidden local, then
 * each binding reads its own piece out of it. */











void compileStatements(AstNode *const *statements,
                              int count) {
  for (int i = 0; i < count; i++) compileNode(statements[i]);
}

/* Pushes a fresh compiler for a nested function and reserves slot 0, which the
 * VM fills with the callee itself. */
void beginFunction(Compiler *compiler, FunctionKind kind, const char *name,
                          int nameLength) {
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->kind = kind;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  current = compiler;

  /* csFunctionNew allocates, and a collection during it would otherwise see a
   * compiler whose `function` field is garbage. */
  /* A function body starts with no open try blocks of its own; an enclosing
   * function's are unreachable from here. */
  compiler->function = csFunctionNew();
  compiler->function->module = currentUnit->module;
  if (kind != FUNCTION_SCRIPT && name != NULL) {
    compiler->function->name = csStringCopy(name, nameLength);
  }

  /* Slot 0 belongs to the running function, and naming it "" keeps it
   * unreachable — except in a method, where it holds the receiver instead. */
  compiler->function->isMethod = isMethodKind(kind);

  Local *local = &compiler->locals[compiler->localCount++];
  local->name = isMethodKind(kind) ? "this" : "";
  local->length = isMethodKind(kind) ? 4 : 0;
  local->depth = 0;
  local->isConst = true;
  local->isCaptured = false;
}

ObjFunction *endFunction(int line) {
  /* Falling off the end of a function returns undefined — except a
   * constructor, which returns the instance it was given. That is what lets
   * OP_NEW leave the instance behind without an opcode of its own. */
  if (current->kind == FUNCTION_CONSTRUCTOR) {
    emitBytes(OP_GET_LOCAL, 0, line);
  } else {
    emitByte(OP_UNDEFINED, line);
  }
  emitByte(OP_RETURN, line);

  ObjFunction *function = current->function;
  current = current->enclosing;
  return function;
}

/* Emits OP_CLOSURE for a function that has just finished compiling, followed
 * by the (isLocal, index) pair per upvalue that tells the VM where each capture
 * comes from. */
void emitClosure(const Compiler *compiler, ObjFunction *function, int line) {
  /* endFunction popped this compiler, so csCompilerMarkRoots no longer reaches
   * the function. It has to stay rooted until the enclosing chunk owns it. */
  csPushTempRoot((Obj *)function);
  emitConstantOp(OP_CLOSURE, makeConstant(OBJ_VAL(function), line), line);
  csPopTempRoot();

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler->upvalues[i].isLocal ? 1 : 0, line);
    emitByte(compiler->upvalues[i].index, line);
  }
}





/* `this.name = <initialiser>;` for each declared field, in declaration order.
 * Emitted straight into whatever function is being compiled — the constructor,
 * or the hidden initialiser below. */










void compileNode(const AstNode *node) {
  if (node == NULL) return;
  int line = node->line;

  switch (node->type) {
    case AST_NUMBER_LITERAL:
      emitConstant(NUMBER_VAL(node->as.number), line);
      break;

    case AST_STRING_LITERAL: {
      ObjString *string = csStringCopy(node->as.string.chars, node->as.string.length);
      emitConstant(OBJ_VAL(string), line);
      break;
    }

    case AST_BOOL_LITERAL:
      emitByte(node->as.boolean ? OP_TRUE : OP_FALSE, line);
      break;

    case AST_NULL_LITERAL:
      emitByte(OP_NULL, line);
      break;

    case AST_UNDEFINED_LITERAL:
      emitByte(OP_UNDEFINED, line);
      break;

    case AST_IDENTIFIER:
      compileIdentifierLoad(node->as.identifier.name,
                            node->as.identifier.length, line);
      break;

    case AST_ASSIGN:
      compileAssign(node, false);
      break;

    case AST_UPDATE:
      compileUpdate(node);
      break;

    case AST_PROPERTY: {
      /* `this.x` and `local.x` fuse the load of the receiver into the read.
       * The pair profile puts this at 8.5% of a class-heavy program. */
      const AstNode *object = node->as.property.object;

      if (isPrivateName(node->as.property.name, node->as.property.length)) {
        compileNode(object);
        emitConstantOp(OP_GET_PRIVATE,
                       identifierConstant(node->as.property.name,
                                          node->as.property.length, line),
                       line);
        break;
      }

      /* The fusion loads the receiver and reads the property in one
       * instruction, which leaves nowhere to test the receiver for `?.`. */
      int slot = object->type == AST_IDENTIFIER && !node->as.property.optional
                     ? resolveLocal(current, object->as.identifier.name,
                                    object->as.identifier.length)
                     : -1;
      if (slot != -1) {
        emitByte(OP_GET_LOCAL_PROPERTY, line);
        emitByte((uint8_t)slot, line);
        emitConstantOperand(
            identifierConstant(node->as.property.name, node->as.property.length, line),
            line);
        int cache = csChunkAddPropertyCache(currentChunk());
        if (cache > UINT16_MAX) errorAt(line, "too many property sites in one function");
        emitConstantOperand(cache, line);
        break;
      }
      compileNode(object);
      if (node->as.property.optional) emitOptionalGuard(line);
      emitPropertyOp(OP_GET_PROPERTY,
                     identifierConstant(node->as.property.name,
                                        node->as.property.length, line),
                     line);
      break;
    }

    case AST_OPTIONAL_CHAIN:
      compileOptionalChain(node);
      break;

    case AST_TEMPLATE_STRINGS:
      compileNode(node->as.templateStrings.cooked);
      compileNode(node->as.templateStrings.raw);
      emitByte(OP_TEMPLATE_STRINGS, line);
      break;

    case AST_SEQUENCE:
      /* The first operand is evaluated only for what it does. */
      compileForEffect(node->as.sequence.first);
      compileNode(node->as.sequence.second);
      break;

    case AST_YIELD:
      if (node->as.yield.isDelegate) {
        /* Delegating needs somewhere to keep its position across each
         * suspension, and a local's slot is the stack height it was declared
         * at — which only holds where nothing else is part-way evaluated. */
        errorAt(line, "'yield*' is only supported as a statement of its own, "
                      "not inside a larger expression");
        break;
      }
      if (node->as.yield.value != NULL) {
        compileNode(node->as.yield.value);
      } else {
        emitByte(OP_UNDEFINED, line);
      }
      emitByte(OP_YIELD, line);
      break;

    case AST_DELETE: {
      const AstNode *target = node->as.deleteTarget;
      if (target->type == AST_PROPERTY) {
        compileNode(target->as.property.object);
        emitConstantOp(OP_DELETE_PROPERTY,
                       identifierConstant(target->as.property.name,
                                          target->as.property.length, line),
                       line);
      } else {
        compileNode(target->as.index.target);
        compileNode(target->as.index.index);
        emitByte(OP_DELETE_INDEX, line);
      }
      break;
    }

    case AST_INDEX:
      compileNode(node->as.index.target);
      /* Before the subscript: `a?.[f()]` must not call `f` when `a` is
       * absent. */
      if (node->as.index.optional) emitOptionalGuard(line);
      compileNode(node->as.index.index);
      emitByte(OP_GET_INDEX, line);
      break;

    case AST_OBJECT_LITERAL: {
      if (node->as.objectLiteral.count > UINT8_MAX) {
        errorAt(line, "too many properties in one object literal (limit %d)",
                UINT8_MAX);
        break;
      }
      /* A spread or an accessor means the entries have to be applied one at a
       * time, in order — the run-of-pairs form can express neither. */
      bool oneAtATime = false;
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        if (node->as.objectLiteral.kinds[i] != OBJECT_ENTRY_VALUE) oneAtATime = true;
      }

      /* Without a spread the whole literal is one instruction over a run of
       * stack pairs. With one it is built up entry by entry, because the
       * entries have to be applied in source order for the last mention of a
       * key to win. */
      if (!oneAtATime) {
        /* Keys and values alternate on the stack; OP_OBJECT consumes the pairs. */
        for (int i = 0; i < node->as.objectLiteral.count; i++) {
          compileNode(node->as.objectLiteral.keys[i]);
          compileNode(node->as.objectLiteral.values[i]);
        }
        emitBytes(OP_OBJECT, (uint8_t)node->as.objectLiteral.count, line);
        break;
      }

      emitBytes(OP_OBJECT, 0, line);
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        ObjectEntryKind kind = (ObjectEntryKind)node->as.objectLiteral.kinds[i];
        if (kind == OBJECT_ENTRY_SPREAD) {
          compileNode(node->as.objectLiteral.values[i]);
          emitByte(OP_OBJECT_MERGE, line);
          continue;
        }

        compileNode(node->as.objectLiteral.keys[i]);
        compileNode(node->as.objectLiteral.values[i]);
        if (kind == OBJECT_ENTRY_VALUE) {
          emitByte(OP_OBJECT_SET, line);
        } else {
          emitBytes(OP_OBJECT_ACCESSOR, kind == OBJECT_ENTRY_GETTER ? 1 : 0, line);
        }
      }
      break;
    }

    case AST_ARRAY_LITERAL: {
      if (node->as.arrayLiteral.count > UINT8_MAX) {
        errorAt(line, "too many elements in one array literal (limit %d)", UINT8_MAX);
        break;
      }

      /* A literal with no spread uses the cheaper builder that copies straight
       * across without inspecting each element. */
      bool hasSpread = false;
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        if (node->as.arrayLiteral.elements[i]->type == AST_SPREAD) hasSpread = true;
        compileNode(node->as.arrayLiteral.elements[i]);
      }
      emitBytes(hasSpread ? OP_ARRAY_SPREAD : OP_ARRAY,
                (uint8_t)node->as.arrayLiteral.count, line);
      break;
    }

    case AST_SPREAD:
      compileNode(node->as.spread);
      emitByte(OP_SPREAD_MARK, line);
      break;

    case AST_DESTRUCTURE:
      compileDestructure(node);
      break;

    case AST_CALL: {
      if (node->as.call.argCount > UINT8_MAX) {
        errorAt(line, "too many arguments (limit %d)", UINT8_MAX);
        break;
      }
      const AstNode *callee = node->as.call.callee;

      /* `super(...)` and `super.m(...)`. Both leave the receiver below the
       * arguments and the superclass on top, which is the shape the two super
       * opcodes read. */
      if (callee->type == AST_SUPER) {
        if (!compileThisLoad(line)) break;
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        if (!compileSuperLoad(line)) break;

        if (callee->as.super.name == NULL) {
          if (current->kind != FUNCTION_CONSTRUCTOR) {
            errorAt(line, "'super()' can only be called from a constructor");
            break;
          }
          emitBytes(OP_SUPER_CALL, (uint8_t)node->as.call.argCount, line);
        } else {
          emitConstantOp(OP_SUPER_INVOKE,
                         identifierConstant(callee->as.super.name,
                                            callee->as.super.length, line),
                         line);
          emitByte((uint8_t)node->as.call.argCount, line);
        }
        break;
      }

      if (node->as.call.isNew) {
        compileNode(callee);
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitBytes(OP_NEW, (uint8_t)node->as.call.argCount, line);
        break;
      }

      bool hasSpread = false;
      for (int i = 0; i < node->as.call.argCount; i++) {
        if (node->as.call.arguments[i]->type == AST_SPREAD) hasSpread = true;
      }

      /* Spread arguments are packed into one array, because how many there are
       * is only known at run time. That costs the receiver, so a built-in
       * method cannot be called this way — `Math.max(...xs)` works because it
       * ignores its receiver, while `xs.push(...ys)` reports that it cannot. */
      if (hasSpread) {
        if (callee->type == AST_PROPERTY) {
          compileNode(callee->as.property.object);
          if (callee->as.property.optional) emitOptionalGuard(line);
          emitPropertyOp(OP_GET_PROPERTY,
                         identifierConstant(callee->as.property.name,
                                            callee->as.property.length, line),
                         line);
        } else {
          compileNode(callee);
        }

        if (node->as.call.optional) emitOptionalGuard(line);
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitBytes(OP_ARRAY_SPREAD, (uint8_t)node->as.call.argCount, line);
        emitByte(OP_CALL_SPREAD, line);
        break;
      }

      /* `x.name(...)` becomes one instruction instead of a property load
       * followed by a call, which also keeps the receiver available so a
       * built-in method can see what it was called on. */
      /* `o.m?.()` tests the method but still calls it on `o`, so the property
       * is read twice: once to see whether it is there, and once by the invoke
       * that keeps the receiver. Loading it as a plain value and calling that
       * would be one lookup, but it would also silently bind `this` to the
       * function — wrong quietly, which is worse than not compiling. */
      if (node->as.call.optional && callee->type == AST_PROPERTY) {
        compileNode(callee->as.property.object);
        if (callee->as.property.optional) emitOptionalGuard(line);

        int nameConstant = identifierConstant(callee->as.property.name,
                                              callee->as.property.length, line);
        emitConstantOp(OP_JUMP_IF_NO_METHOD, nameConstant, line);
        int missing = emitJump16(line);

        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitConstantOp(OP_INVOKE, nameConstant, line);
        emitByte((uint8_t)node->as.call.argCount, line);
        int over = emitJump(OP_JUMP, line);

        /* Absent: the receiver is still on top, and the chain's landing site
         * turns exactly one value into undefined. */
        patchJump(missing, line);
        emitOptionalJump(line);
        patchJump(over, line);
        break;
      }

      bool isMethodCall = callee->type == AST_PROPERTY;
      if (isMethodCall) {
        compileNode(callee->as.property.object);
        if (callee->as.property.optional) emitOptionalGuard(line);
      } else {
        compileNode(callee);
      }
      /* Before the arguments: `f?.(g())` must not call `g` when `f` is
       * absent. Only a plain callee reaches here; the property form is
       * handled above. */
      if (node->as.call.optional) emitOptionalGuard(line);

      for (int i = 0; i < node->as.call.argCount; i++) {
        compileNode(node->as.call.arguments[i]);
      }

      if (isMethodCall) {
        emitConstantOp(OP_INVOKE,
                       identifierConstant(callee->as.property.name,
                                          callee->as.property.length, line),
                       line);
        emitByte((uint8_t)node->as.call.argCount, line);
      } else {
        emitBytes(OP_CALL, (uint8_t)node->as.call.argCount, line);
      }
      break;
    }

    case AST_UNARY:
      compileNode(node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE: emitByte(OP_NEGATE, line); break;
        case UNARY_NOT:    emitByte(OP_NOT, line); break;
        case UNARY_TYPEOF: emitByte(OP_TYPEOF, line); break;
        /* `void x` runs x for whatever it does and answers undefined. */
        case UNARY_VOID:
          emitByte(OP_POP, line);
          emitByte(OP_UNDEFINED, line);
          break;
      }
      break;

    case AST_BINARY:
      compileBinary(node);
      break;

    case AST_LOGICAL:
      compileLogical(node);
      break;

    case AST_GROUPING:
      /* Parentheses only affect parsing; they emit nothing of their own. */
      compileNode(node->as.grouping);
      break;

    case AST_EXPRESSION_STMT:
      /* A statement's value is discarded, which leaves the stack balanced. */
      compileForEffect(node->as.expression);
      break;

    case AST_VAR_DECL:
      compileVarDecl(node);
      break;

    case AST_BLOCK:
      beginScope();
      compileStatements(node->as.block.statements, node->as.block.count);
      endScope(line);
      break;

    case AST_IF_STMT:
      compileIf(node);
      break;

    case AST_WHILE_STMT:
      compileWhile(node);
      break;

    case AST_FOR_STMT:
      compileFor(node);
      break;

    case AST_FOR_OF_STMT:
      compileForOf(node);
      break;

    case AST_FUNCTION:
      compileFunction(node);
      /* A declaration binds the closure to its name; an expression leaves it
       * on the stack for whatever wanted it. An inferred name is not a
       * declaration — the binding it was named after does its own. */
      if (node->as.function.isDeclaration) {
        if (current->scopeDepth > 0) {
          addLocal(node->as.function.name, node->as.function.nameLength, false, line);
        } else {
          addGlobal(node->as.function.name, node->as.function.nameLength, false, line);
          emitConstantOp(OP_DEFINE_GLOBAL, identifierConstant(node->as.function.name,
                                       node->as.function.nameLength, line),
                    line);
        }
      }
      break;

    case AST_CONDITIONAL: {
      int elseJump = emitConditionJump(node->as.conditional.condition, line);
      compileNode(node->as.conditional.thenValue);
      int endJump = emitJump(OP_JUMP, line);
      patchJump(elseJump, line);
      compileNode(node->as.conditional.elseValue);
      patchJump(endJump, line);
      break;
    }

    case AST_BREAK_STMT: {
      Loop *target =
          targetLoop(node->as.jump.label, node->as.jump.labelLength, false, line);
      if (target == NULL) break;
      if (target->breakCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'break' statements in one loop (limit %d)",
                MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(target->scopeDepth, line);
      discardLocalsAbove(target->scopeDepth, line);
      target->breakJumps[target->breakCount++] = emitJump(OP_JUMP, line);
      break;
    }

    case AST_CONTINUE_STMT: {
      Loop *target =
          targetLoop(node->as.jump.label, node->as.jump.labelLength, true, line);
      if (target == NULL) break;
      if (target->continueCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'continue' statements in one loop (limit %d)",
                MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(target->scopeDepth, line);
      discardLocalsAbove(target->scopeDepth, line);
      target->continueJumps[target->continueCount++] = emitJump(OP_JUMP, line);
      break;
    }

    case AST_LABELED_STMT: {
      const AstNode *body = node->as.labeled.body;

      /* A label on a loop or a switch belongs to that construct, so it is
       * handed over for beginLoop to adopt — which is what lets `continue
       * outer` reach the right increment. */
      if (body->type == AST_WHILE_STMT || body->type == AST_FOR_STMT ||
          body->type == AST_FOR_OF_STMT || body->type == AST_SWITCH_STMT) {
        pendingLabel = node->as.labeled.name;
        pendingLabelLength = node->as.labeled.length;
        compileNode(body);
        pendingLabel = NULL;
        pendingLabelLength = 0;
        break;
      }

      /* `outer: { … break outer; }` — a labelled block is a jump target and
       * nothing more, so it gets a context that catches `break` and refuses
       * `continue`, exactly as a switch does. */
      Loop loop;
      pendingLabel = node->as.labeled.name;
      pendingLabelLength = node->as.labeled.length;
      beginLoop(&loop, false);
      compileNode(body);
      endLoop(&loop, line);
      break;
    }

    case AST_SWITCH_STMT: {
      compileNode(node->as.switchStmt.subject);

      Loop loop;
      /* A switch catches `break` but not `continue`, which belongs to any
       * enclosing loop. */
      beginLoop(&loop, false);

      int bodyJumps[MAX_LOOP_EXITS];
      int bodyCount = 0;
      int nextTest = -1;

      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        if (nextTest != -1) patchJump(nextTest, line);

        /* Compare against a copy so the subject survives for the next arm. */
        emitByte(OP_DUP, line);
        compileNode(node->as.switchStmt.cases[i].test);
        emitByte(OP_EQUAL, line);
        nextTest = emitJump(OP_POP_JUMP_IF_FALSE, line);

        if (bodyCount < MAX_LOOP_EXITS) {
          bodyJumps[bodyCount++] = emitJump(OP_JUMP, line);
        }
      }
      if (nextTest != -1) patchJump(nextTest, line);

      /* Nothing matched: fall into `default` if there is one. */
      int afterDefault = -1;
      emitByte(OP_POP, line); /* the subject */
      if (node->as.switchStmt.defaultBody != NULL) {
        beginScope();
        compileStatements(node->as.switchStmt.defaultBody->as.block.statements,
                          node->as.switchStmt.defaultBody->as.block.count);
        endScope(line);
      }
      afterDefault = emitJump(OP_JUMP, line);

      for (int i = 0; i < node->as.switchStmt.caseCount && i < bodyCount; i++) {
        patchJump(bodyJumps[i], line);
        emitByte(OP_POP, line); /* the subject */
        beginScope();
        compileStatements(node->as.switchStmt.cases[i].body->as.block.statements,
                          node->as.switchStmt.cases[i].body->as.block.count);
        endScope(line);
        /* Arms do not fall through, so each one jumps to the end. */
        if (loop.breakCount < MAX_LOOP_EXITS) {
          loop.breakJumps[loop.breakCount++] = emitJump(OP_JUMP, line);
        }
      }

      patchJump(afterDefault, line);
      endLoop(&loop, line);
      break;
    }

    case AST_TRY_STMT:
      compileTry(node);
      break;

    case AST_THROW_STMT:
      compileNode(node->as.thrown);
      emitByte(OP_THROW, line);
      break;

    case AST_RETURN_STMT:
      if (current->kind == FUNCTION_SCRIPT) {
        errorAt(line, "'return' outside of a function");
        break;
      }
      if (current->kind == FUNCTION_CONSTRUCTOR && node->as.returnValue != NULL) {
        errorAt(line, "a constructor cannot return a value; it always yields "
                      "the new instance");
        break;
      }
      /* The value is computed first so the finally blocks run with it already
       * on the stack — they are balanced, so it survives them. */
      if (node->as.returnValue != NULL) {
        compileNode(node->as.returnValue);
      } else if (current->kind == FUNCTION_CONSTRUCTOR) {
        emitBytes(OP_GET_LOCAL, 0, line);
      } else {
        emitByte(OP_UNDEFINED, line);
      }
      unwindTryBlocks(-1, line);
      emitByte(OP_RETURN, line);
      break;

    case AST_AWAIT:
      compileNode(node->as.unary.operand);
      emitAwait(line);
      break;

    case AST_REGEX_LITERAL:
      emitByte(OP_REGEX, line);
      emitConstantOperand(
          identifierConstant(node->as.regex.source, node->as.regex.sourceLength, line),
          line);
      emitConstantOperand(
          identifierConstant(node->as.regex.flags, node->as.regex.flagsLength, line),
          line);
      break;

    case AST_THIS:
      compileThisLoad(line);
      break;

    case AST_SUPER:
      if (node->as.super.name == NULL) {
        errorAt(line, "'super' can only be called or used with a property");
        break;
      }
      /* `super.m` read without calling: the bound method has to carry the
       * receiver with it. */
      if (!compileThisLoad(line)) break;
      if (!compileSuperLoad(line)) break;
      emitConstantOp(OP_GET_SUPER,
                     identifierConstant(node->as.super.name, node->as.super.length, line),
                     line);
      break;

    case AST_CLASS_DECL:
      compileClassDecl(node);
      break;

    case AST_IMPORT:
      compileImport(node);
      break;

    case AST_EXPORT:
      compileExport(node);
      break;

    case AST_PROGRAM:
      compileStatements(node->as.program.statements, node->as.program.count);
      break;
  }
}

ObjFunction *csCompile(AstNode *program, ObjModule *module, Diagnostics *diag) {
  Unit unit;
  unit.diag = diag;
  unit.globalCount = 0;
  unit.module = module;
  currentUnit = &unit;

  Compiler compiler;
  beginFunction(&compiler, FUNCTION_SCRIPT, NULL, 0);

  compileNode(program);
  ObjFunction *function = endFunction(program != NULL ? program->line : 1);

  currentUnit = NULL;
  return csDiagnosticsFailed(diag) ? NULL : function;
}
