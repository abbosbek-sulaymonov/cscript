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

typedef struct {
  Chunk *chunk;
  Diagnostics *diag;

  Local locals[MAX_LOCALS];
  int localCount;
  int scopeDepth; /* 0 means global scope */

  GlobalDecl globals[MAX_GLOBALS];
  int globalCount;
} Compiler;

/* The chunk under construction is a GC root: interning a string constant can
 * allocate, and the constants already written must survive that. */
static Chunk *activeChunk = NULL;

void csCompilerMarkRoots(void) {
  if (activeChunk == NULL) return;
  for (int i = 0; i < activeChunk->constants.count; i++) {
    csMarkValue(activeChunk->constants.values[i]);
  }
}

static void errorAt(Compiler *compiler, int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  /* csDiagnosticError takes the varargs itself, so forward through a buffer. */
  char message[256];
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  csDiagnosticError(compiler->diag, line, NULL, 0, "%s", message);
}

static void emitByte(Compiler *compiler, uint8_t byte, int line) {
  csChunkWrite(compiler->chunk, byte, line);
}

static void emitBytes(Compiler *compiler, uint8_t a, uint8_t b, int line) {
  emitByte(compiler, a, line);
  emitByte(compiler, b, line);
}

/* Adds a value to the constant pool and returns its index, reusing an existing
 * entry when one matches. Identifier names repeat constantly, so deduplicating
 * keeps the pool inside the one-byte operand limit for far longer. */
static int makeConstant(Compiler *compiler, Value value, int line) {
  ValueArray *constants = &compiler->chunk->constants;
  for (int i = 0; i < constants->count; i++) {
    if (csValuesStrictEqual(constants->values[i], value)) return i;
  }

  int index = csChunkAddConstant(compiler->chunk, value);
  if (index > UINT8_MAX) {
    /* A wider OP_CONSTANT_LONG is the fix; not needed at this size yet. */
    errorAt(compiler, line, "too many constants in one chunk (limit %d)", UINT8_MAX + 1);
    return 0;
  }
  return index;
}

static void emitConstant(Compiler *compiler, Value value, int line) {
  emitBytes(compiler, OP_CONSTANT, (uint8_t)makeConstant(compiler, value, line), line);
}

/* Interns an identifier and returns its constant-pool index. */
static uint8_t identifierConstant(Compiler *compiler, const char *name, int length,
                                  int line) {
  ObjString *string = csStringCopy(name, length);
  return (uint8_t)makeConstant(compiler, OBJ_VAL(string), line);
}

/* Writes a jump with a placeholder operand and returns the offset to patch. */
static int emitJump(Compiler *compiler, uint8_t instruction, int line) {
  emitByte(compiler, instruction, line);
  emitByte(compiler, 0xff, line);
  emitByte(compiler, 0xff, line);
  return compiler->chunk->count - 2;
}

/* Fills in a jump emitted earlier, now that the target is known. */
static void patchJump(Compiler *compiler, int offset, int line) {
  int jump = compiler->chunk->count - offset - 2;
  if (jump > UINT16_MAX) {
    errorAt(compiler, line, "jump distance exceeds %d bytes", UINT16_MAX);
    return;
  }
  compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emitLoop(Compiler *compiler, int loopStart, int line) {
  emitByte(compiler, OP_LOOP, line);
  int offset = compiler->chunk->count - loopStart + 2;
  if (offset > UINT16_MAX) {
    errorAt(compiler, line, "loop body is too large to jump back over");
    return;
  }
  emitByte(compiler, (uint8_t)((offset >> 8) & 0xff), line);
  emitByte(compiler, (uint8_t)(offset & 0xff), line);
}

/* ---------------- scope handling ---------------- */

static void beginScope(Compiler *compiler) { compiler->scopeDepth++; }

static void endScope(Compiler *compiler, int line) {
  compiler->scopeDepth--;

  /* Discard the locals that just went out of scope in one instruction rather
   * than emitting a run of OP_POPs. */
  int popped = 0;
  while (compiler->localCount > 0 &&
         compiler->locals[compiler->localCount - 1].depth > compiler->scopeDepth) {
    compiler->localCount--;
    popped++;
  }

  if (popped == 1) {
    emitByte(compiler, OP_POP, line);
  } else if (popped > 1) {
    emitBytes(compiler, OP_POP_N, (uint8_t)popped, line);
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

static void addLocal(Compiler *compiler, const char *name, int length, bool isConst,
                     int line) {
  if (compiler->localCount >= MAX_LOCALS) {
    errorAt(compiler, line, "too many local variables in scope (limit %d)", MAX_LOCALS);
    return;
  }

  /* Redeclaring a name in the same scope is a mistake, not shadowing. */
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (local->depth < compiler->scopeDepth) break;
    if (identifiersEqual(local, name, length)) {
      errorAt(compiler, line, "'%.*s' is already declared in this scope", length, name);
      return;
    }
  }

  Local *local = &compiler->locals[compiler->localCount++];
  local->name = name;
  local->length = length;
  local->depth = compiler->scopeDepth;
  local->isConst = isConst;
}

/* Returns the declaration for a global this unit declared, or NULL. */
static GlobalDecl *findGlobal(Compiler *compiler, const char *name, int length) {
  for (int i = 0; i < compiler->globalCount; i++) {
    GlobalDecl *global = &compiler->globals[i];
    if (global->length == length && memcmp(global->name, name, (size_t)length) == 0) {
      return global;
    }
  }
  return NULL;
}

static void addGlobal(Compiler *compiler, const char *name, int length, bool isConst,
                      int line) {
  if (findGlobal(compiler, name, length) != NULL) {
    errorAt(compiler, line, "'%.*s' is already declared", length, name);
    return;
  }
  if (compiler->globalCount >= MAX_GLOBALS) {
    errorAt(compiler, line, "too many global variables (limit %d)", MAX_GLOBALS);
    return;
  }

  GlobalDecl *global = &compiler->globals[compiler->globalCount++];
  global->name = name;
  global->length = length;
  global->isConst = isConst;
}

/* ---------------- code generation ---------------- */

static void compileNode(Compiler *compiler, const AstNode *node);

static void compileBinary(Compiler *compiler, const AstNode *node) {
  compileNode(compiler, node->as.binary.left);
  compileNode(compiler, node->as.binary.right);

  int line = node->line;

  /* Where the checker resolved both sides to `number`, the generic OP_ADD's
   * string test is dead weight. This is the hook the rest of the specialisation
   * work hangs off: the types are consumed, not erased. */
  bool bothNumbers = node->as.binary.left->resolvedType == TYPE_NUMBER &&
                     node->as.binary.right->resolvedType == TYPE_NUMBER;

  switch (node->as.binary.op) {
    case BINARY_ADD:
      emitByte(compiler, bothNumbers ? OP_ADD_NUM : OP_ADD, line);
      break;
    case BINARY_SUBTRACT:      emitByte(compiler, OP_SUBTRACT, line); break;
    case BINARY_MULTIPLY:      emitByte(compiler, OP_MULTIPLY, line); break;
    case BINARY_DIVIDE:        emitByte(compiler, OP_DIVIDE, line); break;
    case BINARY_MODULO:        emitByte(compiler, OP_MODULO, line); break;
    case BINARY_EQUAL:         emitByte(compiler, OP_EQUAL, line); break;
    case BINARY_NOT_EQUAL:     emitByte(compiler, OP_NOT_EQUAL, line); break;
    case BINARY_GREATER:       emitByte(compiler, OP_GREATER, line); break;
    case BINARY_GREATER_EQUAL: emitByte(compiler, OP_GREATER_EQUAL, line); break;
    case BINARY_LESS:          emitByte(compiler, OP_LESS, line); break;
    case BINARY_LESS_EQUAL:    emitByte(compiler, OP_LESS_EQUAL, line); break;
  }
}

/* && and || evaluate to an operand, not to a boolean, so they compile to a
 * conditional jump that leaves the left value on the stack. */
static void compileLogical(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  compileNode(compiler, node->as.logical.left);

  uint8_t jumpOp =
      node->as.logical.op == LOGICAL_AND ? OP_JUMP_IF_FALSE : OP_JUMP_IF_TRUE;
  int endJump = emitJump(compiler, jumpOp, line);

  /* Not short-circuiting: drop the left value, the right one is the result. */
  emitByte(compiler, OP_POP, line);
  compileNode(compiler, node->as.logical.right);
  patchJump(compiler, endJump, line);
}

static void compileIdentifierLoad(Compiler *compiler, const char *name, int length,
                                  int line) {
  int slot = resolveLocal(compiler, name, length);
  if (slot != -1) {
    emitBytes(compiler, OP_GET_LOCAL, (uint8_t)slot, line);
    return;
  }
  emitBytes(compiler, OP_GET_GLOBAL, identifierConstant(compiler, name, length, line),
            line);
}

static void compileAssign(Compiler *compiler, const AstNode *node) {
  const AstNode *target = node->as.assign.target;
  const char *name = target->as.identifier.name;
  int length = target->as.identifier.length;
  int line = node->line;

  int slot = resolveLocal(compiler, name, length);
  if (slot != -1) {
    if (compiler->locals[slot].isConst) {
      errorAt(compiler, line, "'%.*s' is declared const and cannot be reassigned",
              length, name);
      return;
    }
    compileNode(compiler, node->as.assign.value);
    emitBytes(compiler, OP_SET_LOCAL, (uint8_t)slot, line);
    return;
  }

  GlobalDecl *global = findGlobal(compiler, name, length);
  if (global != NULL && global->isConst) {
    errorAt(compiler, line, "'%.*s' is declared const and cannot be reassigned", length,
            name);
    return;
  }

  compileNode(compiler, node->as.assign.value);
  emitBytes(compiler, OP_SET_GLOBAL, identifierConstant(compiler, name, length, line),
            line);
}

/* ++x / x++ / --x / x--
 *
 * Prefix leaves the updated value; postfix leaves the value from before the
 * update, which is why the old value is duplicated first. */
static void compileUpdate(Compiler *compiler, const AstNode *node) {
  const AstNode *target = node->as.update.target;
  const char *name = target->as.identifier.name;
  int length = target->as.identifier.length;
  int line = node->line;

  int slot = resolveLocal(compiler, name, length);
  if (slot != -1 && compiler->locals[slot].isConst) {
    errorAt(compiler, line, "'%.*s' is declared const and cannot be reassigned", length,
            name);
    return;
  }
  if (slot == -1) {
    GlobalDecl *global = findGlobal(compiler, name, length);
    if (global != NULL && global->isConst) {
      errorAt(compiler, line, "'%.*s' is declared const and cannot be reassigned",
              length, name);
      return;
    }
  }

  uint8_t nameConstant = 0;
  if (slot == -1) nameConstant = identifierConstant(compiler, name, length, line);

  compileIdentifierLoad(compiler, name, length, line);
  if (!node->as.update.isPrefix) emitByte(compiler, OP_DUP, line);

  emitConstant(compiler, NUMBER_VAL(1), line);
  emitByte(compiler, node->as.update.isIncrement ? OP_ADD : OP_SUBTRACT, line);

  if (slot != -1) {
    emitBytes(compiler, OP_SET_LOCAL, (uint8_t)slot, line);
  } else {
    emitBytes(compiler, OP_SET_GLOBAL, nameConstant, line);
  }

  /* The store leaves the new value on top; postfix wants the old one. */
  if (!node->as.update.isPrefix) emitByte(compiler, OP_POP, line);
}

/* Compiles an expression whose value is thrown away.
 *
 * `i++` as a statement is the common case worth special-casing: the general
 * form has to produce the old value, which costs a duplicate and two pops that
 * nothing ever reads. In effect position none of that is observable, so a local
 * update collapses to a single in-place instruction. */
static void compileForEffect(Compiler *compiler, const AstNode *node) {
  if (node != NULL && node->type == AST_UPDATE) {
    const AstNode *target = node->as.update.target;
    const char *name = target->as.identifier.name;
    int length = target->as.identifier.length;
    int slot = resolveLocal(compiler, name, length);

    if (slot != -1 && !compiler->locals[slot].isConst) {
      emitBytes(compiler, node->as.update.isIncrement ? OP_INC_LOCAL : OP_DEC_LOCAL,
                (uint8_t)slot, node->line);
      return;
    }
  }

  compileNode(compiler, node);
  emitByte(compiler, OP_POP, node != NULL ? node->line : 0);
}

static void compileVarDecl(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  const char *name = node->as.varDecl.name;
  int length = node->as.varDecl.length;

  if (node->as.varDecl.initializer != NULL) {
    compileNode(compiler, node->as.varDecl.initializer);
  } else {
    emitByte(compiler, OP_UNDEFINED, line);
  }

  if (compiler->scopeDepth > 0) {
    /* The initialiser's value is already sitting in the slot the local will
     * occupy, so declaring it is pure bookkeeping — no instruction needed. */
    addLocal(compiler, name, length, node->as.varDecl.isConst, line);
    return;
  }

  addGlobal(compiler, name, length, node->as.varDecl.isConst, line);
  emitBytes(compiler, node->as.varDecl.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
            identifierConstant(compiler, name, length, line), line);
}

static void compileIf(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  compileNode(compiler, node->as.ifStmt.condition);

  int thenJump = emitJump(compiler, OP_POP_JUMP_IF_FALSE, line);
  compileNode(compiler, node->as.ifStmt.thenBranch);

  if (node->as.ifStmt.elseBranch == NULL) {
    patchJump(compiler, thenJump, line);
    return;
  }

  int elseJump = emitJump(compiler, OP_JUMP, line);
  patchJump(compiler, thenJump, line);
  compileNode(compiler, node->as.ifStmt.elseBranch);
  patchJump(compiler, elseJump, line);
}

static void compileWhile(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  int loopStart = compiler->chunk->count;

  compileNode(compiler, node->as.whileStmt.condition);
  int exitJump = emitJump(compiler, OP_POP_JUMP_IF_FALSE, line);

  compileNode(compiler, node->as.whileStmt.body);
  emitLoop(compiler, loopStart, line);
  patchJump(compiler, exitJump, line);
}

/* Desugars to a while loop, with the initialiser scoped to the loop so that
 * `for (let i = ...)` cannot leak `i` into the surrounding scope. */
static void compileFor(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  beginScope(compiler);

  if (node->as.forStmt.initializer != NULL) {
    compileNode(compiler, node->as.forStmt.initializer);
  }

  int loopStart = compiler->chunk->count;
  int exitJump = -1;
  if (node->as.forStmt.condition != NULL) {
    compileNode(compiler, node->as.forStmt.condition);
    exitJump = emitJump(compiler, OP_POP_JUMP_IF_FALSE, line);
  }

  compileNode(compiler, node->as.forStmt.body);

  if (node->as.forStmt.increment != NULL) {
    compileForEffect(compiler, node->as.forStmt.increment);
  }

  emitLoop(compiler, loopStart, line);
  if (exitJump != -1) patchJump(compiler, exitJump, line);

  endScope(compiler, line);
}

static void compileStatements(Compiler *compiler, AstNode *const *statements,
                              int count) {
  for (int i = 0; i < count; i++) compileNode(compiler, statements[i]);
}

static void compileNode(Compiler *compiler, const AstNode *node) {
  if (node == NULL) return;
  int line = node->line;

  switch (node->type) {
    case AST_NUMBER_LITERAL:
      emitConstant(compiler, NUMBER_VAL(node->as.number), line);
      break;

    case AST_STRING_LITERAL: {
      ObjString *string = csStringCopy(node->as.string.chars, node->as.string.length);
      emitConstant(compiler, OBJ_VAL(string), line);
      break;
    }

    case AST_BOOL_LITERAL:
      emitByte(compiler, node->as.boolean ? OP_TRUE : OP_FALSE, line);
      break;

    case AST_NULL_LITERAL:
      emitByte(compiler, OP_NULL, line);
      break;

    case AST_UNDEFINED_LITERAL:
      emitByte(compiler, OP_UNDEFINED, line);
      break;

    case AST_IDENTIFIER:
      compileIdentifierLoad(compiler, node->as.identifier.name,
                            node->as.identifier.length, line);
      break;

    case AST_ASSIGN:
      compileAssign(compiler, node);
      break;

    case AST_UPDATE:
      compileUpdate(compiler, node);
      break;

    case AST_PROPERTY:
      compileNode(compiler, node->as.property.object);
      emitBytes(compiler, OP_GET_PROPERTY,
                identifierConstant(compiler, node->as.property.name,
                                   node->as.property.length, line),
                line);
      break;

    case AST_CALL:
      compileNode(compiler, node->as.call.callee);
      for (int i = 0; i < node->as.call.argCount; i++) {
        compileNode(compiler, node->as.call.arguments[i]);
      }
      if (node->as.call.argCount > UINT8_MAX) {
        errorAt(compiler, line, "too many arguments (limit %d)", UINT8_MAX);
        break;
      }
      emitBytes(compiler, OP_CALL, (uint8_t)node->as.call.argCount, line);
      break;

    case AST_UNARY:
      compileNode(compiler, node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE: emitByte(compiler, OP_NEGATE, line); break;
        case UNARY_NOT:    emitByte(compiler, OP_NOT, line); break;
        case UNARY_TYPEOF: emitByte(compiler, OP_TYPEOF, line); break;
      }
      break;

    case AST_BINARY:
      compileBinary(compiler, node);
      break;

    case AST_LOGICAL:
      compileLogical(compiler, node);
      break;

    case AST_GROUPING:
      /* Parentheses only affect parsing; they emit nothing of their own. */
      compileNode(compiler, node->as.grouping);
      break;

    case AST_EXPRESSION_STMT:
      /* A statement's value is discarded, which leaves the stack balanced. */
      compileForEffect(compiler, node->as.expression);
      break;

    case AST_VAR_DECL:
      compileVarDecl(compiler, node);
      break;

    case AST_BLOCK:
      beginScope(compiler);
      compileStatements(compiler, node->as.block.statements, node->as.block.count);
      endScope(compiler, line);
      break;

    case AST_IF_STMT:
      compileIf(compiler, node);
      break;

    case AST_WHILE_STMT:
      compileWhile(compiler, node);
      break;

    case AST_FOR_STMT:
      compileFor(compiler, node);
      break;

    case AST_PROGRAM:
      compileStatements(compiler, node->as.program.statements, node->as.program.count);
      break;
  }
}

bool csCompile(AstNode *program, Chunk *chunk, Diagnostics *diag) {
  Compiler compiler;
  compiler.chunk = chunk;
  compiler.diag = diag;
  compiler.localCount = 0;
  compiler.scopeDepth = 0;
  compiler.globalCount = 0;

  activeChunk = chunk;
  compileNode(&compiler, program);
  emitByte(&compiler, OP_RETURN, program != NULL ? program->line : 1);
  activeChunk = NULL;

  return !csDiagnosticsFailed(diag);
}
