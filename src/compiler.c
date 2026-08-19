#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"

#define MAX_LOCALS 256

/* A variable visible in the current scope. Locals are resolved to a stack slot
 * at compile time, so reading one is an array index rather than a hash lookup —
 * which is the single biggest reason this is a compiler and not a tree walker. */
typedef struct {
  const char *name;
  int length;
  int depth;    /* scope nesting level it was declared at */
  bool isConst;
  bool isCaptured; /* a nested function closed over it */
} Local;

#define MAX_GLOBALS 256

/* A global declared by this compilation unit. Tracked so redeclaring a name is
 * a compile error rather than a silent overwrite, and so assigning to a const
 * is caught before the program ever runs. */
typedef struct {
  const char *name;
  int length;
  bool isConst;
} GlobalDecl;

/* Where a captured variable came from: a local of the immediately enclosing
 * function, or an upvalue that function had already captured itself. */
typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum {
  FUNCTION_SCRIPT, /* the implicit top-level function */
  FUNCTION_BODY,
  /* A method's slot 0 holds the receiver rather than the callee, and is named
   * `this` so an ordinary local lookup finds it — which also means an arrow
   * function inside a method captures it as an upvalue and gets JavaScript's
   * lexical `this` without any special rule. */
  FUNCTION_METHOD,
  FUNCTION_CONSTRUCTOR,
} FunctionKind;

static bool isMethodKind(FunctionKind kind) {
  return kind == FUNCTION_METHOD || kind == FUNCTION_CONSTRUCTOR;
}

/* One per function being compiled. They form a stack through `enclosing`, which
 * is what lets an inner function resolve a name to an outer function's local. */
typedef struct Compiler {
  struct Compiler *enclosing;
  ObjFunction *function;
  FunctionKind kind;

  Local locals[MAX_LOCALS];
  int localCount;
  int scopeDepth; /* 0 is the function's own top level */

  Upvalue upvalues[MAX_LOCALS];
} Compiler;

/* Globals are shared across every function in a compilation unit, so the
 * declaration table lives outside the per-function compiler. */
typedef struct {
  Diagnostics *diag;
  GlobalDecl globals[MAX_GLOBALS];
  int globalCount;
} Unit;

#define MAX_LOOP_EXITS 64

/* The innermost enclosing loop or switch, so `break` and `continue` know where
 * to go. Jumps out are recorded here and patched once the exit point is known. */
typedef struct Loop {
  struct Loop *enclosing;
  int scopeDepth;      /* locals above this are discarded when jumping out */
  int continueTarget;  /* -1 while unknown, e.g. a for-loop's increment */
  bool allowsContinue; /* false inside a switch */

  int breakJumps[MAX_LOOP_EXITS];
  int breakCount;
  int continueJumps[MAX_LOOP_EXITS];
  int continueCount;
} Loop;

/* An enclosing `try` whose `finally` any jump out of it has to run first.
 *
 * `return`, `break` and `continue` all leave a try block without reaching the
 * end of its body, and JavaScript still runs the finally on each of those
 * paths. Tracking the open try blocks is what lets the compiler emit it. */
typedef struct TryContext {
  struct TryContext *enclosing;
  const AstNode *finallyBody; /* NULL for a try with only a catch */
  int scopeDepth;
  /* False while the catch block is being compiled: the handler was already
   * removed on entry to the catch, so jumping out of it must run the finally
   * but must not pop a handler that is no longer installed. */
  bool handlerActive;
} TryContext;

static Compiler *current = NULL;
static Unit *currentUnit = NULL;
static Loop *currentLoop = NULL;
static TryContext *currentTry = NULL;

static Chunk *currentChunk(void) { return &current->function->chunk; }

/* Every function still being compiled is a root: interning a string constant
 * allocates, and the constants already written must survive that. Nested
 * functions mean walking the whole chain, not just the innermost. */
void csCompilerMarkRoots(void) {
  for (Compiler *compiler = current; compiler != NULL;
       compiler = compiler->enclosing) {
    csMarkObject((Obj *)compiler->function);
  }
}

static void errorAt(int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  /* csDiagnosticError takes the varargs itself, so forward through a buffer. */
  char message[256];
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  currentUnit->diag->panicMode = false;
  csDiagnosticError(currentUnit->diag, line, NULL, 0, "%s", message);
}

static void emitByte(uint8_t byte, int line) {
  csChunkWrite(currentChunk(), byte, line);
}

static void emitBytes(uint8_t a, uint8_t b, int line) {
  emitByte(a, line);
  emitByte(b, line);
}

/* Constant-pool indices are 16-bit, big-endian, matching the jump offsets. */
static void emitConstantOperand(int index, int line) {
  emitByte((uint8_t)((index >> 8) & 0xff), line);
  emitByte((uint8_t)(index & 0xff), line);
}

/* Emits an opcode whose single operand is a constant index. */
static void emitConstantOp(uint8_t opcode, int index, int line) {
  emitByte(opcode, line);
  emitConstantOperand(index, line);
}

/* Opcodes that carry an inline cache take a second 16-bit operand: the index
 * of this site's entry in the chunk's cache array. Every site gets its own, so
 * one `o.x` in a loop never fights with a different `o.x` elsewhere. */
static void emitPropertyOp(uint8_t opcode, int index, int line) {
  emitConstantOp(opcode, index, line);
  int cache = csChunkAddPropertyCache(currentChunk());
  if (cache > UINT16_MAX) errorAt(line, "too many property sites in one function");
  emitConstantOperand(cache, line);
}

static void emitGlobalOp(uint8_t opcode, int index, int line) {
  emitConstantOp(opcode, index, line);
  int cache = csChunkAddGlobalCache(currentChunk());
  if (cache > UINT16_MAX) errorAt(line, "too many global sites in one function");
  emitConstantOperand(cache, line);
}

/* Adds a value to the constant pool and returns its index, reusing an existing
 * entry when one matches. Identifier names repeat constantly, so deduplicating
 * keeps the pool inside the one-byte operand limit for far longer. */
static int makeConstant(Value value, int line) {
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

static void emitConstant(Value value, int line) {
  emitConstantOp(OP_CONSTANT, makeConstant(value, line), line);
}

/* Interns an identifier and returns its constant-pool index. */
static int identifierConstant(const char *name, int length, int line) {
  ObjString *string = csStringCopy(name, length);
  return makeConstant(OBJ_VAL(string), line);
}

/* Writes a jump with a placeholder operand and returns the offset to patch. */
static int emitJump(uint8_t instruction, int line) {
  emitByte(instruction, line);
  emitByte(0xff, line);
  emitByte(0xff, line);
  return currentChunk()->count - 2;
}

/* Fills in a jump emitted earlier, now that the target is known. */
static void patchJump(int offset, int line) {
  int jump = currentChunk()->count - offset - 2;
  if (jump > UINT16_MAX) {
    errorAt(line, "jump distance exceeds %d bytes", UINT16_MAX);
    return;
  }
  currentChunk()->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  currentChunk()->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emitLoop(int loopStart, int line) {
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

static void beginScope(void) { current->scopeDepth++; }

static void endScope(int line) {
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

static bool identifiersEqual(const Local *local, const char *name, int length) {
  return local->length == length && memcmp(local->name, name, (size_t)length) == 0;
}

/* Returns the stack slot for a local, or -1 when the name is not local. */
static int resolveLocal(Compiler *compiler, const char *name, int length) {
  /* Search backwards so an inner declaration shadows an outer one. */
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    if (identifiersEqual(&compiler->locals[i], name, length)) return i;
  }
  return -1;
}

/* Records that `compiler` captures a variable, reusing the slot if it already
 * captured the same one. */
static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal, int line) {
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
static int resolveUpvalue(Compiler *compiler, const char *name, int length, int line) {
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
static bool enclosingLocalIsConst(Compiler *compiler, const char *name, int length) {
  for (Compiler *scope = compiler->enclosing; scope != NULL; scope = scope->enclosing) {
    int slot = resolveLocal(scope, name, length);
    if (slot != -1) return scope->locals[slot].isConst;
  }
  return false;
}

static void addLocal(const char *name, int length, bool isConst,
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
static GlobalDecl *findGlobal(const char *name, int length) {
  for (int i = 0; i < currentUnit->globalCount; i++) {
    GlobalDecl *global = &currentUnit->globals[i];
    if (global->length == length && memcmp(global->name, name, (size_t)length) == 0) {
      return global;
    }
  }
  return NULL;
}

static void addGlobal(const char *name, int length, bool isConst,
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

/* Emits the pops needed to leave every scope inside `depth` before jumping out
 * of a loop. The locals stay in the compiler's table, because the code after
 * the jump is still inside the loop and can still see them. */
static void discardLocalsAbove(int depth, int line) {
  int count = 0;
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth <= depth) break;
    /* A captured local has to be closed individually, so the fast path is
     * abandoned as soon as one appears. */
    if (current->locals[i].isCaptured) {
      if (count == 1) {
        emitByte(OP_POP, line);
      } else if (count > 1) {
        emitBytes(OP_POP_N, (uint8_t)count, line);
      }
      count = 0;
      emitByte(OP_CLOSE_UPVALUE, line);
      continue;
    }
    count++;
  }

  if (count == 1) {
    emitByte(OP_POP, line);
  } else if (count > 1) {
    emitBytes(OP_POP_N, (uint8_t)count, line);
  }
}

static void beginLoop(Loop *loop, bool allowsContinue) {
  loop->enclosing = currentLoop;
  loop->scopeDepth = current->scopeDepth;
  loop->continueTarget = -1;
  loop->allowsContinue = allowsContinue;
  loop->breakCount = 0;
  loop->continueCount = 0;
  currentLoop = loop;
}

static void endLoop(Loop *loop, int line) {
  for (int i = 0; i < loop->breakCount; i++) {
    patchJump(loop->breakJumps[i], line);
  }
  currentLoop = loop->enclosing;
}

/* Emits the finally blocks for every open try down to `stopAtDepth`, closing
 * each handler on the way. Passing -1 means "all of them in this function",
 * which is what a `return` needs; a `break` stops at the loop's own depth. */
static void compileNode(const AstNode *node);

static void unwindTryBlocks(int stopAtDepth, int line) {
  for (TryContext *context = currentTry; context != NULL;
       context = context->enclosing) {
    if (context->scopeDepth <= stopAtDepth) break;
    if (context->handlerActive) emitByte(OP_END_TRY, line);
    if (context->finallyBody != NULL) compileNode(context->finallyBody);
  }
}

/* ---------------- code generation ---------------- */

static void compileNode(const AstNode *node);

/* Emits a binary operator's two operands, fusing "a local, then a literal" into
 * one instruction. That shape covers `i < n`, `i % 7` and `total + 1`, and
 * profiling put it at 14-18% of everything executed in loop-heavy code. */
static void compileOperandPair(const AstNode *left, const AstNode *right, int line) {
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

  compileNode(left);
  compileNode(right);
}

static void compileBinary(const AstNode *node) {
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
static void compileLogical(const AstNode *node) {
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

static void compileIdentifierLoad(const char *name, int length, int line) {
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
static void compileAssign(const AstNode *node, bool discard) {
  const AstNode *target = node->as.assign.target;
  int assignLine = node->line;

  if (target->type == AST_PROPERTY) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitPropertyOp(OP_SET_PROPERTY, identifierConstant(target->as.property.name, target->as.property.length,
                                 assignLine),
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
static void compileUpdate(const AstNode *node) {
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
static void compileForEffect(const AstNode *node) {
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
      node->as.assign.target->type == AST_IDENTIFIER) {
    compileAssign(node, true);
    return;
  }

  compileNode(node);
  emitByte(OP_POP, node != NULL ? node->line : 0);
}

/* `const [a, b] = xs;` and `const { x, y } = o;`
 *
 * Compiles to the loads and stores the pattern stands for, so nothing new
 * exists at run time. The source is evaluated once into a hidden local, then
 * each binding reads its own piece out of it. */
static void compileDestructure(const AstNode *node) {
  int line = node->line;
  bool isObject = node->as.destructure.isObject;

  /* The source, held in a slot the body cannot name. */
  compileNode(node->as.destructure.initializer);
  addLocal(" source", 7, true, line);
  int sourceSlot = current->localCount - 1;

  for (int i = 0; i < node->as.destructure.count; i++) {
    const AstBinding *binding = &node->as.destructure.bindings[i];

    if (binding->isRest) {
      emitBytes(OP_GET_LOCAL, (uint8_t)sourceSlot, line);
      emitBytes(OP_ARRAY_REST, (uint8_t)i, line);
    } else {
      emitBytes(OP_GET_LOCAL, (uint8_t)sourceSlot, line);
      if (isObject) {
        emitPropertyOp(OP_GET_PROPERTY,
                       identifierConstant(binding->key, binding->keyLength, line),
                       line);
      } else {
        emitConstant(NUMBER_VAL(i), line);
        emitByte(OP_GET_INDEX, line);
      }

      /* A default applies when the piece is missing, which is what undefined
       * means for both a short array and an absent property. */
      if (binding->defaultValue != NULL) {
        emitByte(OP_DUP, line);
        emitByte(OP_UNDEFINED, line);
        emitByte(OP_NOT_EQUAL, line);
        int keepJump = emitJump(OP_POP_JUMP_IF_FALSE, line);
        int doneJump = emitJump(OP_JUMP, line);
        patchJump(keepJump, line);
        emitByte(OP_POP, line); /* the undefined */
        compileNode(binding->defaultValue);
        patchJump(doneJump, line);
      }
    }

    /* The value is already sitting where the binding's slot will be. */
    if (current->scopeDepth > 0) {
      addLocal(binding->name, binding->nameLength, node->as.destructure.isConst, line);
    } else {
      addGlobal(binding->name, binding->nameLength, node->as.destructure.isConst, line);
      emitConstantOp(node->as.destructure.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
                     identifierConstant(binding->name, binding->nameLength, line), line);
    }
  }

  /* At global scope the hidden source local is the only thing still on the
   * stack; inside a block it stays put and the enclosing scope discards it. */
  if (current->scopeDepth == 0) {
    current->localCount--;
    emitByte(OP_POP, line);
  }
}

static void compileVarDecl(const AstNode *node) {
  int line = node->line;
  const char *name = node->as.varDecl.name;
  int length = node->as.varDecl.length;

  if (node->as.varDecl.initializer != NULL) {
    compileNode(node->as.varDecl.initializer);
  } else {
    emitByte(OP_UNDEFINED, line);
  }

  if (current->scopeDepth > 0) {
    /* The initialiser's value is already sitting in the slot the local will
     * occupy, so declaring it is pure bookkeeping — no instruction needed. */
    addLocal(name, length, node->as.varDecl.isConst, line);
    return;
  }

  addGlobal(name, length, node->as.varDecl.isConst, line);
  emitConstantOp(node->as.varDecl.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
                 identifierConstant(name, length, line), line);
}

/* Maps a comparison to the fused jump that tests it directly. The jump is
 * taken when the comparison is false, so each opcode is its negation. */
static bool fusedConditionJump(const AstNode *condition, uint8_t *opcode) {
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
static int emitConditionJump(const AstNode *condition, int line) {
  uint8_t fused;
  if (fusedConditionJump(condition, &fused)) {
    compileOperandPair(condition->as.binary.left, condition->as.binary.right, line);
    return emitJump(fused, line);
  }

  compileNode(condition);
  return emitJump(OP_POP_JUMP_IF_FALSE, line);
}

static void compileIf(const AstNode *node) {
  int line = node->line;
  int thenJump = emitConditionJump(node->as.ifStmt.condition, line);
  compileNode(node->as.ifStmt.thenBranch);

  if (node->as.ifStmt.elseBranch == NULL) {
    patchJump(thenJump, line);
    return;
  }

  int elseJump = emitJump(OP_JUMP, line);
  patchJump(thenJump, line);
  compileNode(node->as.ifStmt.elseBranch);
  patchJump(elseJump, line);
}

static void compileWhile(const AstNode *node) {
  int line = node->line;
  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  int exitJump = emitConditionJump(node->as.whileStmt.condition, line);

  compileNode(node->as.whileStmt.body);

  /* `continue` re-tests the condition. */
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i], line);
  }
  emitLoop(loopStart, line);
  patchJump(exitJump, line);
  endLoop(&loop, line);
}

/* True when a subtree contains a function, which is the only way a program can
 * observe whether a loop variable is one binding or one per iteration.
 *
 * Used to decide whether a `for` loop needs the per-iteration copy below. A
 * loop with no closures in it keeps the single-slot form and stays fast. */
static bool containsFunction(const AstNode *node) {
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

/* Desugars to a while loop, with the initialiser scoped to the loop so that
 * `for (let i = ...)` cannot leak `i` into the surrounding scope.
 *
 * When the body contains a closure, each iteration gets its *own* binding of
 * the loop variable — a shadowing copy made on entry and written back on exit.
 * JavaScript specifies that for `let`, and it is the difference between
 * collecting three closures that return 0, 1, 2 and three that all return 3.
 * The copy is skipped entirely when no closure can observe it. */
static void compileFor(const AstNode *node) {
  int line = node->line;
  beginScope();

  if (node->as.forStmt.initializer != NULL) {
    compileNode(node->as.forStmt.initializer);
  }

  /* Per-iteration binding is only observable through a closure, so the copy is
   * emitted only when the loop actually contains one. */
  bool perIteration = node->as.forStmt.initializer != NULL &&
                      node->as.forStmt.initializer->type == AST_VAR_DECL &&
                      !node->as.forStmt.initializer->as.varDecl.isConst &&
                      containsFunction(node->as.forStmt.body);
  int outerSlot = perIteration ? current->localCount - 1 : -1;
  const char *bindingName =
      perIteration ? node->as.forStmt.initializer->as.varDecl.name : NULL;
  int bindingLength =
      perIteration ? node->as.forStmt.initializer->as.varDecl.length : 0;

  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  int exitJump = -1;
  if (node->as.forStmt.condition != NULL) {
    exitJump = emitConditionJump(node->as.forStmt.condition, line);
  }

  int innerSlot = -1;
  if (perIteration) {
    /* A shadowing copy, so the body — and anything it closes over — sees a
     * binding that belongs to this iteration alone. */
    beginScope();
    emitBytes(OP_GET_LOCAL, (uint8_t)outerSlot, line);
    addLocal(bindingName, bindingLength, false, line);
    innerSlot = current->localCount - 1;
  }

  compileNode(node->as.forStmt.body);

  /* `continue` in a for-loop must still run the increment, so it lands here
   * rather than at the condition — skipping it would spin forever. It also has
   * to reach the write-back, or the iteration's changes would be lost. */
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i], line);
  }

  if (perIteration) {
    /* Copy the iteration's value back before the shared slot is advanced. */
    emitBytes(OP_GET_LOCAL, (uint8_t)innerSlot, line);
    emitByte(OP_SET_LOCAL_POP, line);
    emitByte((uint8_t)outerSlot, line);
    endScope(line);
  }

  if (node->as.forStmt.increment != NULL) {
    compileForEffect(node->as.forStmt.increment);
  }

  emitLoop(loopStart, line);
  if (exitJump != -1) patchJump(exitJump, line);
  endLoop(&loop, line);

  endScope(line);
}

/* `for (const x of xs) body` becomes an index loop over two hidden locals: the
 * iterable itself and a counter. They are given names no source can produce, so
 * the body cannot see or shadow them.
 *
 * There is no iterator protocol yet — arrays and strings are the only iterable
 * things — so this stays a desugaring rather than a runtime mechanism. */
static void compileForOf(const AstNode *node) {
  int line = node->line;
  beginScope();

  /* Slot 1: the iterable, evaluated once. */
  compileNode(node->as.forOf.iterable);
  addLocal(" iterable", 9, true, line);
  int iterableSlot = current->localCount - 1;

  /* Slot 2: the index, starting at zero. */
  emitConstant(NUMBER_VAL(0), line);
  addLocal(" index", 6, false, line);
  int indexSlot = current->localCount - 1;

  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  /* index < length(iterable). ITER_LENGTH replaces the iterable with its
   * length, leaving exactly the two operands the fused compare wants. The
   * length is recomputed each pass, so a body that appends is seen. */
  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot, line);
  emitBytes(OP_GET_LOCAL, (uint8_t)iterableSlot, line);
  emitByte(OP_ITER_LENGTH, line);
  int exitJump = emitJump(OP_JUMP_IF_NOT_LESS, line);

  /* The binding is a fresh local per iteration, so a closure made in the body
   * captures that iteration's value rather than sharing one cell. */
  beginScope();
  emitBytes(OP_GET_LOCAL, (uint8_t)iterableSlot, line);
  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot, line);
  emitByte(OP_GET_INDEX, line);
  addLocal(node->as.forOf.name, node->as.forOf.nameLength, node->as.forOf.isConst,
           line);

  compileNode(node->as.forOf.body);
  endScope(line);

  for (int i = 0; i < loop.continueCount; i++) patchJump(loop.continueJumps[i], line);
  emitBytes(OP_INC_LOCAL, (uint8_t)indexSlot, line);
  emitLoop(loopStart, line);

  patchJump(exitJump, line);
  endLoop(&loop, line);
  endScope(line);
}

/* try / catch / finally.
 *
 * The shape emitted is:
 *
 *     OP_TRY -> catch                install a handler
 *     <body>
 *     OP_END_TRY                     normal path: remove it
 *     <finally>                      run on the way out
 *     OP_JUMP -> end
 *   catch:                           the handler resumes here with the thrown
 *     <bind or discard the value>    value on top of the stack
 *     <catch body>
 *     <finally>
 *   end:
 *
 * `finally` is emitted twice rather than jumped to, because the two paths
 * arrive with different stacks and have to leave differently. Duplicating a
 * block is the cost of not needing a subroutine-return mechanism the VM does
 * not otherwise have.
 *
 * With no catch, the handler still points at the finally, which runs and then
 * rethrows so the exception keeps travelling. */
static void compileTry(const AstNode *node) {
  int line = node->line;
  bool hasCatch = node->as.tryStmt.catchBody != NULL;
  bool hasFinally = node->as.tryStmt.finallyBody != NULL;

  int handlerJump = emitJump(OP_TRY, line);

  TryContext context;
  context.enclosing = currentTry;
  context.finallyBody = node->as.tryStmt.finallyBody;
  context.scopeDepth = current->scopeDepth;
  context.handlerActive = true;
  currentTry = &context;

  compileNode(node->as.tryStmt.body);

  currentTry = context.enclosing;
  emitByte(OP_END_TRY, line);

  if (hasFinally) compileNode(node->as.tryStmt.finallyBody);
  int endJump = emitJump(OP_JUMP, line);

  /* The handler resumes here with the thrown value on top of the stack. */
  patchJump(handlerJump, line);

  if (hasCatch) {
    /* The finally still covers the catch block, so a return out of it has to
     * run the finally — but the handler is already gone by this point. */
    context.handlerActive = false;
    currentTry = &context;

    beginScope();
    if (node->as.tryStmt.catchName != NULL) {
      /* The thrown value is already in the slot the binding will occupy. */
      addLocal(node->as.tryStmt.catchName, node->as.tryStmt.catchNameLength, false,
               line);
    } else {
      emitByte(OP_POP, line); /* `catch { }` ignores the value */
    }
    compileNode(node->as.tryStmt.catchBody);
    endScope(line);

    currentTry = context.enclosing;
    if (hasFinally) compileNode(node->as.tryStmt.finallyBody);
  } else {
    /* finally-only: run the block, then let the exception continue. */
    compileNode(node->as.tryStmt.finallyBody);
    emitByte(OP_THROW, line);
  }

  patchJump(endJump, line);
}

static void compileStatements(AstNode *const *statements,
                              int count) {
  for (int i = 0; i < count; i++) compileNode(statements[i]);
}

/* Pushes a fresh compiler for a nested function and reserves slot 0, which the
 * VM fills with the callee itself. */
static void beginFunction(Compiler *compiler, FunctionKind kind, const char *name,
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
  if (kind != FUNCTION_SCRIPT && name != NULL) {
    compiler->function->name = csStringCopy(name, nameLength);
  }

  /* Slot 0 belongs to the running function, and naming it "" keeps it
   * unreachable — except in a method, where it holds the receiver instead. */
  Local *local = &compiler->locals[compiler->localCount++];
  local->name = isMethodKind(kind) ? "this" : "";
  local->length = isMethodKind(kind) ? 4 : 0;
  local->depth = 0;
  local->isConst = true;
  local->isCaptured = false;
}

static ObjFunction *endFunction(int line) {
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
static void emitClosure(const Compiler *compiler, ObjFunction *function, int line) {
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

static void compileFunctionAs(const AstNode *node, FunctionKind kind) {
  int line = node->line;

  /* A nested function's `return` must not emit the enclosing function's
   * finally blocks — they belong to a frame it will never unwind. */
  TryContext *enclosingTry = currentTry;
  currentTry = NULL;

  Compiler compiler;
  beginFunction(&compiler, kind, node->as.function.name,
                node->as.function.nameLength);
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

  compileStatements(node->as.function.body->as.block.statements,
                    node->as.function.body->as.block.count);

  /* No endScope(): the whole frame is discarded by OP_RETURN, so popping the
   * locals first would be wasted work. */
  ObjFunction *function = endFunction(line);
  currentTry = enclosingTry;
  emitClosure(&compiler, function, line);
}

static void compileFunction(const AstNode *node) {
  compileFunctionAs(node, FUNCTION_BODY);
}

/* Pushes `this`, which is slot 0 of the nearest enclosing method — directly
 * when compiling that method, and through the upvalue machinery from an arrow
 * function nested inside it. */
static bool compileThisLoad(int line) {
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
static bool compileSuperLoad(int line) {
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

/* `this.name = <initialiser>;` for each declared field, in declaration order.
 * Emitted straight into whatever function is being compiled — the constructor,
 * or the hidden initialiser below. */
static void emitFieldAssignments(const AstNode *node) {
  for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
    const AstClassField *field = &node->as.classDecl.fields[i];
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
static void compileFieldInitializer(const AstNode *node) {
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
static bool isSuperCallStatement(const AstNode *statement) {
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
static void compileConstructor(const AstNode *classNode) {
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

static void compileClassDecl(const AstNode *node) {
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
  } else if (node->as.classDecl.fieldCount > 0) {
    compileFieldInitializer(node);
    emitByte(OP_FIELD_INIT, line);
  }

  for (int i = 0; i < node->as.classDecl.memberCount; i++) {
    const AstClassMember *member = &node->as.classDecl.members[i];
    compileFunctionAs(member->function, FUNCTION_METHOD);
    emitConstantOp(member->isStatic ? OP_STATIC_METHOD : OP_METHOD,
                   identifierConstant(member->function->as.function.name,
                                      member->function->as.function.nameLength, line),
                   line);
  }

  emitByte(OP_POP, line);
  if (hasSuper) endScope(line);
}

static void compileNode(const AstNode *node) {
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

    case AST_PROPERTY:
      compileNode(node->as.property.object);
      emitPropertyOp(OP_GET_PROPERTY, identifierConstant(node->as.property.name, node->as.property.length,
                                   line),
                line);
      break;

    case AST_INDEX:
      compileNode(node->as.index.target);
      compileNode(node->as.index.index);
      emitByte(OP_GET_INDEX, line);
      break;

    case AST_OBJECT_LITERAL: {
      if (node->as.objectLiteral.count > UINT8_MAX) {
        errorAt(line, "too many properties in one object literal (limit %d)",
                UINT8_MAX);
        break;
      }
      /* Keys and values alternate on the stack; OP_OBJECT consumes the pairs. */
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        compileNode(node->as.objectLiteral.keys[i]);
        compileNode(node->as.objectLiteral.values[i]);
      }
      emitBytes(OP_OBJECT, (uint8_t)node->as.objectLiteral.count, line);
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
          emitPropertyOp(OP_GET_PROPERTY,
                         identifierConstant(callee->as.property.name,
                                            callee->as.property.length, line),
                         line);
        } else {
          compileNode(callee);
        }

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
      bool isMethodCall = callee->type == AST_PROPERTY;
      if (isMethodCall) {
        compileNode(callee->as.property.object);
      } else {
        compileNode(callee);
      }

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
      if (node->as.function.name != NULL && !node->as.function.nameIsInferred) {
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
      if (currentLoop == NULL) {
        errorAt(line, "'break' outside of a loop or switch");
        break;
      }
      if (currentLoop->breakCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'break' statements in one loop (limit %d)",
                MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(currentLoop->scopeDepth, line);
      discardLocalsAbove(currentLoop->scopeDepth, line);
      currentLoop->breakJumps[currentLoop->breakCount++] = emitJump(OP_JUMP, line);
      break;
    }

    case AST_CONTINUE_STMT: {
      if (currentLoop == NULL || !currentLoop->allowsContinue) {
        errorAt(line, "'continue' outside of a loop");
        break;
      }
      if (currentLoop->continueCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'continue' statements in one loop (limit %d)",
                MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(currentLoop->scopeDepth, line);
      discardLocalsAbove(currentLoop->scopeDepth, line);
      currentLoop->continueJumps[currentLoop->continueCount++] = emitJump(OP_JUMP, line);
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

    case AST_PROGRAM:
      compileStatements(node->as.program.statements, node->as.program.count);
      break;
  }
}

ObjFunction *csCompile(AstNode *program, Diagnostics *diag) {
  Unit unit;
  unit.diag = diag;
  unit.globalCount = 0;
  currentUnit = &unit;

  Compiler compiler;
  beginFunction(&compiler, FUNCTION_SCRIPT, NULL, 0);

  compileNode(program);
  ObjFunction *function = endFunction(program != NULL ? program->line : 1);

  currentUnit = NULL;
  return csDiagnosticsFailed(diag) ? NULL : function;
}
