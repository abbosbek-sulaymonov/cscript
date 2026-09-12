/* compiler.c — the AST to bytecode, and everything the emission rests on.
 *
 * The emit helpers, scope and local resolution, upvalue capture, and the
 * dispatcher that sends each node to the half of the compiler that knows about
 * it. What each node *becomes* is in compiler_node_value.c and
 * compiler_node_statement.c; the larger forms have files of their own —
 * compiler_expression.c, compiler_statement.c, compiler_class.c,
 * compiler_module.c — and compiler_internal.h is what they share.
 */
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
#include "compiler/compiler_internal.h"

/* The compiler's ambient state. Declared in compiler_internal.h; defined here
 * so there is exactly one of each. */
Compiler *current = NULL;
Unit *currentUnit = NULL;
Loop *currentLoop = NULL;
TryContext *currentTry = NULL;

Chunk *currentChunk(void) {
  return &current->function->chunk;
}

/* Every function still being compiled is a root: interning a string constant
 * allocates, and the constants already written must survive that. Nested
 * functions mean walking the whole chain, not just the innermost. */
void csCompilerMarkRoots(void) {
  for (Compiler *compiler = current; compiler != NULL; compiler = compiler->enclosing) {
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

/* The two-byte placeholder alone, for a jump whose opcode and other operands
 * are already written — OP_JUMP_IF_NO_METHOD carries a constant first. */
int emitJump16(int line) {
  emitByte(0xff, line);
  emitByte(0xff, line);
  return currentChunk()->count - 2;
}

/* Writes a jump with a placeholder operand and returns the offset to patch. */
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

void beginScope(void) {
  current->scopeDepth++;
}

void endScope(int line) {
  current->scopeDepth--;

  /* Locals that were captured cannot simply be popped: a closure may outlive
   * this scope and still refer to them, so those are moved onto the heap
   * individually. Everything else is discarded in one instruction. */
  int pending = 0;
  while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
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

void addLocal(const char *name, int length, bool isConst, int line) {
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

void addGlobal(const char *name, int length, bool isConst, int line) {
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
Loop *compilerTargetLoop(const char *label, int labelLength, bool needsContinue, int line) {
  for (Loop *loop = currentLoop; loop != NULL; loop = loop->enclosing) {
    if (label == NULL) {
      if (!needsContinue || loop->allowsContinue) return loop;
      continue;
    }
    if (loop->label == NULL || loop->labelLength != labelLength || memcmp(loop->label, label, (size_t)labelLength) != 0) {
      continue;
    }
    if (needsContinue && !loop->allowsContinue) {
      errorAt(line, "'continue %.*s' names a label that is not on a loop", labelLength, label);
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

void compileStatements(AstNode *const *statements, int count) {
  for (int i = 0; i < count; i++) compileNode(statements[i]);
}

/* Pushes a fresh compiler for a nested function and reserves slot 0, which the
 * VM fills with the callee itself. */
void beginFunction(Compiler *compiler, FunctionKind kind, const char *name, int nameLength) {
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
  /* Everything but the top level and an arrow owns a `this` in slot 0. */
  bool ownsThis = kind != FUNCTION_SCRIPT && kind != FUNCTION_ARROW;
  local->name = ownsThis ? "this" : "";
  local->length = ownsThis ? 4 : 0;
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

/* Sends a node to whichever half of the compiler knows about it.
 *
 * Two halves rather than one switch because there were ninety cases in it, and
 * the split is the one the language already makes: an expression leaves a
 * value on the stack and a statement leaves the stack as it found it. Asked in
 * that order because expressions are the common case, and a node neither
 * claims is a node the parser should never have built. */
void compileNode(const AstNode *node) {
  if (node == NULL) return;
  int line = node->line;

  if (compileValueNode(node, line)) return;
  if (compileStatementNode(node, line)) return;
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
