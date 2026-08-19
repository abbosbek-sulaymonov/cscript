#include <stdlib.h>
#include <string.h>

#include "cscript/ast.h"

/* The arena hands out bump-allocated slices of large blocks. Nodes are small
 * and numerous, and they all die at the same moment, so per-node free() would
 * be pure overhead. */
#define AST_BLOCK_SIZE (64 * 1024)

struct AstArenaBlock {
  struct AstArenaBlock *next;
  size_t used;
  size_t capacity;
  char data[];
};

void csAstArenaInit(AstArena *arena) {
  arena->head = NULL;
  arena->bytesAllocated = 0;
}

void csAstArenaFree(AstArena *arena) {
  AstArenaBlock *block = arena->head;
  while (block != NULL) {
    AstArenaBlock *next = block->next;
    free(block);
    block = next;
  }
  csAstArenaInit(arena);
}

static AstArenaBlock *newBlock(size_t capacity) {
  AstArenaBlock *block = (AstArenaBlock *)malloc(sizeof(AstArenaBlock) + capacity);
  if (block == NULL) return NULL;
  block->next = NULL;
  block->used = 0;
  block->capacity = capacity;
  return block;
}

void *csAstArenaAlloc(AstArena *arena, size_t size) {
  /* Keep every allocation pointer-aligned. */
  size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);

  if (arena->head == NULL || arena->head->used + size > arena->head->capacity) {
    /* An oversized request gets a block of its own rather than wasting one. */
    size_t capacity = size > AST_BLOCK_SIZE ? size : AST_BLOCK_SIZE;
    AstArenaBlock *block = newBlock(capacity);
    if (block == NULL) return NULL;
    block->next = arena->head;
    arena->head = block;
  }

  void *result = arena->head->data + arena->head->used;
  arena->head->used += size;
  arena->bytesAllocated += size;
  return result;
}

static AstNode *newNode(AstArena *arena, AstNodeType type, int line) {
  AstNode *node = (AstNode *)csAstArenaAlloc(arena, sizeof(AstNode));
  if (node == NULL) return NULL;
  memset(node, 0, sizeof(AstNode));
  node->type = type;
  node->line = line;
  return node;
}

AstNode *csAstNumber(AstArena *arena, int line, double value) {
  AstNode *node = newNode(arena, AST_NUMBER_LITERAL, line);
  if (node != NULL) node->as.number = value;
  return node;
}

AstNode *csAstString(AstArena *arena, int line, const char *chars, int length) {
  AstNode *node = newNode(arena, AST_STRING_LITERAL, line);
  if (node == NULL) return NULL;

  /* Copy into the arena: the decoded text differs from the source slice once
   * escapes are resolved, and it must outlive the parse loop. */
  char *copy = (char *)csAstArenaAlloc(arena, (size_t)length + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, chars, (size_t)length);
  copy[length] = '\0';

  node->as.string.chars = copy;
  node->as.string.length = length;
  return node;
}

AstNode *csAstBool(AstArena *arena, int line, bool value) {
  AstNode *node = newNode(arena, AST_BOOL_LITERAL, line);
  if (node != NULL) node->as.boolean = value;
  return node;
}

AstNode *csAstNull(AstArena *arena, int line) {
  return newNode(arena, AST_NULL_LITERAL, line);
}

AstNode *csAstUndefined(AstArena *arena, int line) {
  return newNode(arena, AST_UNDEFINED_LITERAL, line);
}

AstNode *csAstUnary(AstArena *arena, int line, UnaryOp op, AstNode *operand) {
  AstNode *node = newNode(arena, AST_UNARY, line);
  if (node == NULL) return NULL;
  node->as.unary.op = op;
  node->as.unary.operand = operand;
  return node;
}

AstNode *csAstBinary(AstArena *arena, int line, BinaryOp op, AstNode *left,
                     AstNode *right) {
  AstNode *node = newNode(arena, AST_BINARY, line);
  if (node == NULL) return NULL;
  node->as.binary.op = op;
  node->as.binary.left = left;
  node->as.binary.right = right;
  return node;
}

AstNode *csAstLogical(AstArena *arena, int line, LogicalOp op, AstNode *left,
                      AstNode *right) {
  AstNode *node = newNode(arena, AST_LOGICAL, line);
  if (node == NULL) return NULL;
  node->as.logical.op = op;
  node->as.logical.left = left;
  node->as.logical.right = right;
  return node;
}

AstNode *csAstGrouping(AstArena *arena, int line, AstNode *inner) {
  AstNode *node = newNode(arena, AST_GROUPING, line);
  if (node != NULL) node->as.grouping = inner;
  return node;
}

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = newNode(arena, AST_EXPRESSION_STMT, line);
  if (node != NULL) node->as.expression = expression;
  return node;
}

AstNode *csAstPrintStmt(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = newNode(arena, AST_PRINT_STMT, line);
  if (node != NULL) node->as.expression = expression;
  return node;
}

AstNode *csAstProgram(AstArena *arena, int line) {
  AstNode *node = newNode(arena, AST_PROGRAM, line);
  if (node == NULL) return NULL;
  node->as.program.statements = NULL;
  node->as.program.count = 0;
  node->as.program.capacity = 0;
  return node;
}

void csAstProgramAdd(AstArena *arena, AstNode *program, AstNode *statement) {
  if (program == NULL || statement == NULL) return;

  if (program->as.program.count + 1 > program->as.program.capacity) {
    int oldCapacity = program->as.program.capacity;
    int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;

    /* The arena cannot resize in place, so grow by copying. Statement lists are
     * short and this doubles, so the wasted space stays bounded. */
    AstNode **grown =
        (AstNode **)csAstArenaAlloc(arena, sizeof(AstNode *) * (size_t)newCapacity);
    if (grown == NULL) return;
    if (program->as.program.statements != NULL) {
      memcpy(grown, program->as.program.statements,
             sizeof(AstNode *) * (size_t)oldCapacity);
    }
    program->as.program.statements = grown;
    program->as.program.capacity = newCapacity;
  }

  program->as.program.statements[program->as.program.count++] = statement;
}

const char *csUnaryOpName(UnaryOp op) {
  switch (op) {
    case UNARY_NEGATE: return "-";
    case UNARY_NOT:    return "!";
    case UNARY_TYPEOF: return "typeof";
  }
  return "?";
}

const char *csBinaryOpName(BinaryOp op) {
  switch (op) {
    case BINARY_ADD:              return "+";
    case BINARY_SUBTRACT:         return "-";
    case BINARY_MULTIPLY:         return "*";
    case BINARY_DIVIDE:           return "/";
    case BINARY_MODULO:           return "%";
    case BINARY_EQUAL:            return "==";
    case BINARY_NOT_EQUAL:        return "!=";
    case BINARY_STRICT_EQUAL:     return "===";
    case BINARY_STRICT_NOT_EQUAL: return "!==";
    case BINARY_GREATER:          return ">";
    case BINARY_GREATER_EQUAL:    return ">=";
    case BINARY_LESS:             return "<";
    case BINARY_LESS_EQUAL:       return "<=";
  }
  return "?";
}

const char *csLogicalOpName(LogicalOp op) {
  switch (op) {
    case LOGICAL_AND: return "&&";
    case LOGICAL_OR:  return "||";
  }
  return "?";
}
