#include <stdio.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/vm.h"

typedef struct {
  Chunk *chunk;
  Diagnostics *diag;
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

static void emitByte(Compiler *compiler, uint8_t byte, int line) {
  csChunkWrite(compiler->chunk, byte, line);
}

static void emitBytes(Compiler *compiler, uint8_t a, uint8_t b, int line) {
  emitByte(compiler, a, line);
  emitByte(compiler, b, line);
}

static void emitConstant(Compiler *compiler, Value value, int line) {
  int index = csChunkAddConstant(compiler->chunk, value);
  if (index > UINT8_MAX) {
    /* A wider OP_CONSTANT_LONG is the fix; not needed at this size yet. */
    csDiagnosticError(compiler->diag, line, NULL, 0,
                      "too many constants in one chunk (limit %d)", UINT8_MAX + 1);
    return;
  }
  emitBytes(compiler, OP_CONSTANT, (uint8_t)index, line);
}

/* Writes a jump with a placeholder operand and returns the offset to patch. */
static int emitJump(Compiler *compiler, uint8_t instruction, int line) {
  emitByte(compiler, instruction, line);
  emitByte(compiler, 0xff, line);
  emitByte(compiler, 0xff, line);
  return compiler->chunk->count - 2;
}

/* Fills in a jump emitted earlier, now that the target is known. */
static void patchJump(Compiler *compiler, int offset) {
  int jump = compiler->chunk->count - offset - 2;
  if (jump > UINT16_MAX) {
    csDiagnosticError(compiler->diag, 0, NULL, 0, "jump distance exceeds %d bytes",
                      UINT16_MAX);
    return;
  }
  compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void compileNode(Compiler *compiler, const AstNode *node);

static void compileBinary(Compiler *compiler, const AstNode *node) {
  compileNode(compiler, node->as.binary.left);
  compileNode(compiler, node->as.binary.right);

  int line = node->line;
  switch (node->as.binary.op) {
    case BINARY_ADD:              emitByte(compiler, OP_ADD, line); break;
    case BINARY_SUBTRACT:         emitByte(compiler, OP_SUBTRACT, line); break;
    case BINARY_MULTIPLY:         emitByte(compiler, OP_MULTIPLY, line); break;
    case BINARY_DIVIDE:           emitByte(compiler, OP_DIVIDE, line); break;
    case BINARY_MODULO:           emitByte(compiler, OP_MODULO, line); break;
    case BINARY_EQUAL:            emitByte(compiler, OP_EQUAL, line); break;
    case BINARY_NOT_EQUAL:        emitByte(compiler, OP_NOT_EQUAL, line); break;
    case BINARY_STRICT_EQUAL:     emitByte(compiler, OP_STRICT_EQUAL, line); break;
    case BINARY_STRICT_NOT_EQUAL: emitByte(compiler, OP_STRICT_NOT_EQUAL, line); break;
    case BINARY_GREATER:          emitByte(compiler, OP_GREATER, line); break;
    case BINARY_GREATER_EQUAL:    emitByte(compiler, OP_GREATER_EQUAL, line); break;
    case BINARY_LESS:             emitByte(compiler, OP_LESS, line); break;
    case BINARY_LESS_EQUAL:       emitByte(compiler, OP_LESS_EQUAL, line); break;
  }
}

/* && and || short-circuit and evaluate to an operand, not to a boolean, so they
 * compile to a conditional jump that leaves the left value on the stack. */
static void compileLogical(Compiler *compiler, const AstNode *node) {
  int line = node->line;
  compileNode(compiler, node->as.logical.left);

  uint8_t jumpOp = node->as.logical.op == LOGICAL_AND ? OP_JUMP_IF_FALSE
                                                      : OP_JUMP_IF_TRUE;
  int endJump = emitJump(compiler, jumpOp, line);

  /* Not short-circuiting: drop the left value, the right one is the result. */
  emitByte(compiler, OP_POP, line);
  compileNode(compiler, node->as.logical.right);
  patchJump(compiler, endJump);
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
      compileNode(compiler, node->as.expression);
      emitByte(compiler, OP_POP, line); /* statements leave the stack balanced */
      break;

    case AST_PRINT_STMT:
      compileNode(compiler, node->as.expression);
      emitByte(compiler, OP_PRINT, line);
      break;

    case AST_PROGRAM:
      for (int i = 0; i < node->as.program.count; i++) {
        compileNode(compiler, node->as.program.statements[i]);
      }
      break;
  }
}

bool csCompile(AstNode *program, Chunk *chunk, Diagnostics *diag) {
  Compiler compiler;
  compiler.chunk = chunk;
  compiler.diag = diag;

  activeChunk = chunk;
  compileNode(&compiler, program);
  emitByte(&compiler, OP_RETURN, program != NULL ? program->line : 1);
  activeChunk = NULL;

  return !csDiagnosticsFailed(diag);
}
