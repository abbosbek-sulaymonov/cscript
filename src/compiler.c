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
} FunctionKind;

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

static Compiler *current = NULL;
static Unit *currentUnit = NULL;
static Loop *currentLoop = NULL;

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

/* Adds a value to the constant pool and returns its index, reusing an existing
 * entry when one matches. Identifier names repeat constantly, so deduplicating
 * keeps the pool inside the one-byte operand limit for far longer. */
static int makeConstant(Value value, int line) {
  ValueArray *constants = &currentChunk()->constants;
  for (int i = 0; i < constants->count; i++) {
    if (csValuesStrictEqual(constants->values[i], value)) return i;
  }

  int index = csChunkAddConstant(currentChunk(), value);
  if (index > UINT8_MAX) {
    /* A wider OP_CONSTANT_LONG is the fix; not needed at this size yet. */
    errorAt(line, "too many constants in one chunk (limit %d)", UINT8_MAX + 1);
    return 0;
  }
  return index;
}

static void emitConstant(Value value, int line) {
  emitBytes(OP_CONSTANT, (uint8_t)makeConstant(value, line), line);
}

/* Interns an identifier and returns its constant-pool index. */
static uint8_t identifierConstant(const char *name, int length,
                                  int line) {
  ObjString *string = csStringCopy(name, length);
  return (uint8_t)makeConstant(OBJ_VAL(string), line);
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

  /* Redeclaring a name in the same scope is a mistake, not shadowing. */
  for (int i = current->localCount - 1; i >= 0; i--) {
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
      emitByte((uint8_t)makeConstant(NUMBER_VAL(right->as.number), line), line);
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
    case BINARY_EQUAL:         emitByte(OP_EQUAL, line); break;
    case BINARY_NOT_EQUAL:     emitByte(OP_NOT_EQUAL, line); break;
    case BINARY_GREATER:       emitByte(OP_GREATER, line); break;
    case BINARY_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL, line); break;
    case BINARY_LESS:          emitByte(OP_LESS, line); break;
    case BINARY_LESS_EQUAL:    emitByte(OP_LESS_EQUAL, line); break;
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

  emitBytes(OP_GET_GLOBAL, identifierConstant(name, length, line), line);
}

/* `discard` is set when the assignment's value is thrown away, which lets the
 * store and the pop fuse into one instruction. */
static void compileAssign(const AstNode *node, bool discard) {
  const AstNode *target = node->as.assign.target;
  int assignLine = node->line;

  if (target->type == AST_PROPERTY) {
    compileNode(target->as.property.object);
    compileNode(node->as.assign.value);
    emitBytes(OP_SET_PROPERTY,
              identifierConstant(target->as.property.name, target->as.property.length,
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
  emitBytes(discard ? OP_SET_GLOBAL_POP : OP_SET_GLOBAL,
            identifierConstant(name, length, line), line);
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
  uint8_t nameConstant = 0;
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
    emitBytes(OP_SET_GLOBAL, nameConstant, line);
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
  emitBytes(node->as.varDecl.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
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

/* Desugars to a while loop, with the initialiser scoped to the loop so that
 * `for (let i = ...)` cannot leak `i` into the surrounding scope. */
static void compileFor(const AstNode *node) {
  int line = node->line;
  beginScope();

  if (node->as.forStmt.initializer != NULL) {
    compileNode(node->as.forStmt.initializer);
  }

  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  int exitJump = -1;
  if (node->as.forStmt.condition != NULL) {
    exitJump = emitConditionJump(node->as.forStmt.condition, line);
  }

  compileNode(node->as.forStmt.body);

  /* `continue` in a for-loop must still run the increment, so it lands here
   * rather than at the condition — skipping it would spin forever. */
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i], line);
  }

  if (node->as.forStmt.increment != NULL) {
    compileForEffect(node->as.forStmt.increment);
  }

  emitLoop(loopStart, line);
  if (exitJump != -1) patchJump(exitJump, line);
  endLoop(&loop, line);

  endScope(line);
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
  compiler->function = csFunctionNew();
  if (kind != FUNCTION_SCRIPT && name != NULL) {
    compiler->function->name = csStringCopy(name, nameLength);
  }

  /* Slot 0 belongs to the running function; naming it "" keeps it unreachable. */
  Local *local = &compiler->locals[compiler->localCount++];
  local->name = "";
  local->length = 0;
  local->depth = 0;
  local->isConst = true;
  local->isCaptured = false;
}

static ObjFunction *endFunction(int line) {
  /* Falling off the end of a function returns undefined. */
  emitByte(OP_UNDEFINED, line);
  emitByte(OP_RETURN, line);

  ObjFunction *function = current->function;
  current = current->enclosing;
  return function;
}

static void compileFunction(const AstNode *node) {
  int line = node->line;

  Compiler compiler;
  beginFunction(&compiler, FUNCTION_BODY, node->as.function.name,
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

  /* endFunction popped this compiler, so csCompilerMarkRoots no longer reaches
   * the function. It has to stay rooted until the enclosing chunk owns it. */
  csPushTempRoot((Obj *)function);
  emitBytes(OP_CLOSURE, (uint8_t)makeConstant(OBJ_VAL(function), line), line);
  csPopTempRoot();

  /* Each upvalue is described by the pair of bytes following OP_CLOSURE, so the
   * VM can wire it up at run time. */
  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0, line);
    emitByte(compiler.upvalues[i].index, line);
  }
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
      emitBytes(OP_GET_PROPERTY,
                identifierConstant(node->as.property.name, node->as.property.length,
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
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        compileNode(node->as.arrayLiteral.elements[i]);
      }
      emitBytes(OP_ARRAY, (uint8_t)node->as.arrayLiteral.count, line);
      break;
    }

    case AST_CALL:
      compileNode(node->as.call.callee);
      for (int i = 0; i < node->as.call.argCount; i++) {
        compileNode(node->as.call.arguments[i]);
      }
      if (node->as.call.argCount > UINT8_MAX) {
        errorAt(line, "too many arguments (limit %d)", UINT8_MAX);
        break;
      }
      emitBytes(OP_CALL, (uint8_t)node->as.call.argCount, line);
      break;

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

    case AST_FUNCTION:
      compileFunction(node);
      /* A declaration binds the closure to its name; an expression leaves it
       * on the stack for whatever wanted it. */
      if (node->as.function.name != NULL) {
        if (current->scopeDepth > 0) {
          addLocal(node->as.function.name, node->as.function.nameLength, false, line);
        } else {
          addGlobal(node->as.function.name, node->as.function.nameLength, false, line);
          emitBytes(OP_DEFINE_GLOBAL,
                    identifierConstant(node->as.function.name,
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

    case AST_RETURN_STMT:
      if (current->kind == FUNCTION_SCRIPT) {
        errorAt(line, "'return' outside of a function");
        break;
      }
      if (node->as.returnValue != NULL) {
        compileNode(node->as.returnValue);
      } else {
        emitByte(OP_UNDEFINED, line);
      }
      emitByte(OP_RETURN, line);
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
