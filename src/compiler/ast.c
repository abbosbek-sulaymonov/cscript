/* ast.c — the arena the tree lives in, and the expression nodes.
 *
 * Nodes are never freed individually: the arena is reset or destroyed as a
 * unit once compilation is done, which is what lets every constructor here be
 * three lines and no destructor exist at all. A node holds pointers into the
 * source text rather than copies of it, so the source has to outlive the
 * tree — and it does, because the diagnostics quote from it.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/ast.h"

#include "compiler/ast_internal.h"

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

AstNode *csAstNewNode(AstArena *arena, AstNodeType type, int line) {
  AstNode *node = (AstNode *)csAstArenaAlloc(arena, sizeof(AstNode));
  if (node == NULL) return NULL;
  memset(node, 0, sizeof(AstNode));
  node->type = type;
  node->line = line;
  node->resolvedType = TYPE_DYNAMIC; /* until the checker says otherwise */
  return node;
}

AstNode *csAstNumber(AstArena *arena, int line, double value) {
  AstNode *node = csAstNewNode(arena, AST_NUMBER_LITERAL, line);
  if (node != NULL) node->as.number = value;
  return node;
}

/* Shares the string node's payload: a BigInt literal travels as the digits it
 * was written with, and is only turned into a number by the compiler, which is
 * the first place there is a heap to put one on. */
AstNode *csAstBigInt(AstArena *arena, int line, const char *chars, int length) {
  AstNode *node = csAstString(arena, line, chars, length);
  if (node != NULL) node->type = AST_BIGINT_LITERAL;
  return node;
}

AstNode *csAstString(AstArena *arena, int line, const char *chars, int length) {
  AstNode *node = csAstNewNode(arena, AST_STRING_LITERAL, line);
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
  AstNode *node = csAstNewNode(arena, AST_BOOL_LITERAL, line);
  if (node != NULL) node->as.boolean = value;
  return node;
}

AstNode *csAstNull(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_NULL_LITERAL, line);
}

AstNode *csAstUndefined(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_UNDEFINED_LITERAL, line);
}

AstNode *csAstUnary(AstArena *arena, int line, UnaryOp op, AstNode *operand) {
  AstNode *node = csAstNewNode(arena, AST_UNARY, line);
  if (node == NULL) return NULL;
  node->as.unary.op = op;
  node->as.unary.operand = operand;
  return node;
}

AstNode *csAstBinary(AstArena *arena, int line, BinaryOp op, AstNode *left,
                     AstNode *right) {
  AstNode *node = csAstNewNode(arena, AST_BINARY, line);
  if (node == NULL) return NULL;
  node->as.binary.op = op;
  node->as.binary.left = left;
  node->as.binary.right = right;
  return node;
}

AstNode *csAstLogical(AstArena *arena, int line, LogicalOp op, AstNode *left,
                      AstNode *right) {
  AstNode *node = csAstNewNode(arena, AST_LOGICAL, line);
  if (node == NULL) return NULL;
  node->as.logical.op = op;
  node->as.logical.left = left;
  node->as.logical.right = right;
  return node;
}

AstNode *csAstGrouping(AstArena *arena, int line, AstNode *inner) {
  AstNode *node = csAstNewNode(arena, AST_GROUPING, line);
  if (node != NULL) node->as.grouping = inner;
  return node;
}

/* Copies a NUL-terminated name into the arena so it outlives the token. */
const char *csAstInternName(AstArena *arena, const char *name, int length,
                              int *lengthOut) {
  char *copy = (char *)csAstArenaAlloc(arena, (size_t)length + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, name, (size_t)length);
  copy[length] = '\0';
  *lengthOut = length;
  return copy;
}

AstNode *csAstIdentifier(AstArena *arena, int line, const char *name, int length) {
  AstNode *node = csAstNewNode(arena, AST_IDENTIFIER, line);
  if (node == NULL) return NULL;
  node->as.identifier.name =
      csAstInternName(arena, name, length, &node->as.identifier.length);
  return node;
}

AstNode *csAstAssign(AstArena *arena, int line, AstNode *target, AstNode *value) {
  AstNode *node = csAstNewNode(arena, AST_ASSIGN, line);
  if (node == NULL) return NULL;
  node->as.assign.target = target;
  node->as.assign.value = value;
  return node;
}

AstNode *csAstAssignKind(AstArena *arena, int line, AstNode *target,
                         AstNode *value, AssignKind kind, BinaryOp compoundOp) {
  AstNode *node = csAstAssign(arena, line, target, value);
  if (node == NULL) return NULL;
  node->as.assign.kind = kind;
  node->as.assign.compoundOp = compoundOp;
  return node;
}

AstNode *csAstTemplateStrings(AstArena *arena, int line, AstNode *cooked,
                              AstNode *raw) {
  AstNode *node = csAstNewNode(arena, AST_TEMPLATE_STRINGS, line);
  if (node == NULL) return NULL;
  node->as.templateStrings.cooked = cooked;
  node->as.templateStrings.raw = raw;
  return node;
}

AstNode *csAstSequence(AstArena *arena, int line, AstNode *first, AstNode *second) {
  AstNode *node = csAstNewNode(arena, AST_SEQUENCE, line);
  if (node == NULL) return NULL;
  node->as.sequence.first = first;
  node->as.sequence.second = second;
  return node;
}

AstNode *csAstYield(AstArena *arena, int line, AstNode *value, bool isDelegate) {
  AstNode *node = csAstNewNode(arena, AST_YIELD, line);
  if (node == NULL) return NULL;
  node->as.yield.value = value;
  node->as.yield.isDelegate = isDelegate;
  return node;
}

AstNode *csAstDelete(AstArena *arena, int line, AstNode *target) {
  AstNode *node = csAstNewNode(arena, AST_DELETE, line);
  if (node == NULL) return NULL;
  node->as.deleteTarget = target;
  return node;
}

AstNode *csAstOptionalChain(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = csAstNewNode(arena, AST_OPTIONAL_CHAIN, line);
  if (node == NULL) return NULL;
  node->as.expression = expression;
  return node;
}

AstNode *csAstUpdate(AstArena *arena, int line, AstNode *target, bool isIncrement,
                     bool isPrefix) {
  AstNode *node = csAstNewNode(arena, AST_UPDATE, line);
  if (node == NULL) return NULL;
  node->as.update.target = target;
  node->as.update.isIncrement = isIncrement;
  node->as.update.isPrefix = isPrefix;
  return node;
}

AstNode *csAstCall(AstArena *arena, int line, AstNode *callee) {
  AstNode *node = csAstNewNode(arena, AST_CALL, line);
  if (node == NULL) return NULL;
  node->as.call.callee = callee;
  node->as.call.arguments = NULL;
  node->as.call.argCount = 0;
  node->as.call.isNew = false;
  return node;
}

void csAstCallAddArgument(AstArena *arena, AstNode *call, AstNode *argument) {
  if (call == NULL || argument == NULL) return;

  /* Argument lists are tiny, so a fresh copy per append is cheaper than
   * carrying a capacity field around. */
  AstNode **grown = (AstNode **)csAstArenaAlloc(
      arena, sizeof(AstNode *) * (size_t)(call->as.call.argCount + 1));
  if (grown == NULL) return;
  if (call->as.call.arguments != NULL) {
    memcpy(grown, call->as.call.arguments,
           sizeof(AstNode *) * (size_t)call->as.call.argCount);
  }
  grown[call->as.call.argCount] = argument;
  call->as.call.arguments = grown;
  call->as.call.argCount++;
}

AstNode *csAstProperty(AstArena *arena, int line, AstNode *object, const char *name,
                       int length) {
  AstNode *node = csAstNewNode(arena, AST_PROPERTY, line);
  if (node == NULL) return NULL;
  node->as.property.object = object;
  node->as.property.name = csAstInternName(arena, name, length, &node->as.property.length);
  return node;
}

AstNode *csAstVarDecl(AstArena *arena, int line, const char *name, int length,
                      AstNode *initializer, bool isConst, TypeKind declaredType,
                      bool hasAnnotation) {
  AstNode *node = csAstNewNode(arena, AST_VAR_DECL, line);
  if (node == NULL) return NULL;
  node->as.varDecl.name = csAstInternName(arena, name, length, &node->as.varDecl.length);
  node->as.varDecl.initializer = initializer;
  node->as.varDecl.isConst = isConst;
  node->as.varDecl.declaredType = declaredType;
  node->as.varDecl.hasAnnotation = hasAnnotation;
  return node;
}
