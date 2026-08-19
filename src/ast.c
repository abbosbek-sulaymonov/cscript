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
  node->resolvedType = TYPE_ANY; /* until the checker says otherwise */
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

/* Copies a NUL-terminated name into the arena so it outlives the token. */
static const char *internName(AstArena *arena, const char *name, int length,
                              int *lengthOut) {
  char *copy = (char *)csAstArenaAlloc(arena, (size_t)length + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, name, (size_t)length);
  copy[length] = '\0';
  *lengthOut = length;
  return copy;
}

AstNode *csAstIdentifier(AstArena *arena, int line, const char *name, int length) {
  AstNode *node = newNode(arena, AST_IDENTIFIER, line);
  if (node == NULL) return NULL;
  node->as.identifier.name =
      internName(arena, name, length, &node->as.identifier.length);
  return node;
}

AstNode *csAstAssign(AstArena *arena, int line, AstNode *target, AstNode *value) {
  AstNode *node = newNode(arena, AST_ASSIGN, line);
  if (node == NULL) return NULL;
  node->as.assign.target = target;
  node->as.assign.value = value;
  return node;
}

AstNode *csAstUpdate(AstArena *arena, int line, AstNode *target, bool isIncrement,
                     bool isPrefix) {
  AstNode *node = newNode(arena, AST_UPDATE, line);
  if (node == NULL) return NULL;
  node->as.update.target = target;
  node->as.update.isIncrement = isIncrement;
  node->as.update.isPrefix = isPrefix;
  return node;
}

AstNode *csAstCall(AstArena *arena, int line, AstNode *callee) {
  AstNode *node = newNode(arena, AST_CALL, line);
  if (node == NULL) return NULL;
  node->as.call.callee = callee;
  node->as.call.arguments = NULL;
  node->as.call.argCount = 0;
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
  AstNode *node = newNode(arena, AST_PROPERTY, line);
  if (node == NULL) return NULL;
  node->as.property.object = object;
  node->as.property.name = internName(arena, name, length, &node->as.property.length);
  return node;
}

AstNode *csAstVarDecl(AstArena *arena, int line, const char *name, int length,
                      AstNode *initializer, bool isConst, TypeKind declaredType,
                      bool hasAnnotation) {
  AstNode *node = newNode(arena, AST_VAR_DECL, line);
  if (node == NULL) return NULL;
  node->as.varDecl.name = internName(arena, name, length, &node->as.varDecl.length);
  node->as.varDecl.initializer = initializer;
  node->as.varDecl.isConst = isConst;
  node->as.varDecl.declaredType = declaredType;
  node->as.varDecl.hasAnnotation = hasAnnotation;
  return node;
}

AstNode *csAstBlock(AstArena *arena, int line) {
  AstNode *node = newNode(arena, AST_BLOCK, line);
  if (node == NULL) return NULL;
  node->as.block.statements = NULL;
  node->as.block.count = 0;
  node->as.block.capacity = 0;
  return node;
}

AstNode *csAstIf(AstArena *arena, int line, AstNode *condition, AstNode *thenBranch,
                 AstNode *elseBranch) {
  AstNode *node = newNode(arena, AST_IF_STMT, line);
  if (node == NULL) return NULL;
  node->as.ifStmt.condition = condition;
  node->as.ifStmt.thenBranch = thenBranch;
  node->as.ifStmt.elseBranch = elseBranch;
  return node;
}

AstNode *csAstWhile(AstArena *arena, int line, AstNode *condition, AstNode *body) {
  AstNode *node = newNode(arena, AST_WHILE_STMT, line);
  if (node == NULL) return NULL;
  node->as.whileStmt.condition = condition;
  node->as.whileStmt.body = body;
  return node;
}

AstNode *csAstFor(AstArena *arena, int line, AstNode *initializer, AstNode *condition,
                  AstNode *increment, AstNode *body) {
  AstNode *node = newNode(arena, AST_FOR_STMT, line);
  if (node == NULL) return NULL;
  node->as.forStmt.initializer = initializer;
  node->as.forStmt.condition = condition;
  node->as.forStmt.increment = increment;
  node->as.forStmt.body = body;
  return node;
}

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = newNode(arena, AST_EXPRESSION_STMT, line);
  if (node != NULL) node->as.expression = expression;
  return node;
}

AstNode *csAstFunction(AstArena *arena, int line, const char *name, int nameLength) {
  AstNode *node = newNode(arena, AST_FUNCTION, line);
  if (node == NULL) return NULL;
  node->as.function.name =
      name != NULL ? internName(arena, name, nameLength, &node->as.function.nameLength)
                   : NULL;
  if (name == NULL) node->as.function.nameLength = 0;
  node->as.function.params = NULL;
  node->as.function.paramCount = 0;
  node->as.function.body = NULL;
  node->as.function.returnType = TYPE_ANY;
  node->as.function.hasReturnAnnotation = false;
  return node;
}

void csAstFunctionAddParam(AstArena *arena, AstNode *function, const char *name,
                           int length, TypeKind type, bool hasAnnotation) {
  if (function == NULL) return;

  /* Parameter lists are short, so growing by copy costs less than carrying a
   * capacity field on every function node. */
  int count = function->as.function.paramCount;
  AstParam *grown = (AstParam *)csAstArenaAlloc(arena, sizeof(AstParam) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (function->as.function.params != NULL) {
    memcpy(grown, function->as.function.params, sizeof(AstParam) * (size_t)count);
  }

  int stored = 0;
  grown[count].name = internName(arena, name, length, &stored);
  grown[count].length = stored;
  grown[count].type = type;
  grown[count].hasAnnotation = hasAnnotation;

  function->as.function.params = grown;
  function->as.function.paramCount = count + 1;
}

/* Grows an arena-backed pointer list by copying. The lists here are short, so
 * this costs less than carrying a capacity field on every node. */
static AstNode **growList(AstArena *arena, AstNode **list, int count) {
  AstNode **grown =
      (AstNode **)csAstArenaAlloc(arena, sizeof(AstNode *) * (size_t)(count + 1));
  if (grown == NULL) return NULL;
  if (list != NULL) memcpy(grown, list, sizeof(AstNode *) * (size_t)count);
  return grown;
}

AstNode *csAstConditional(AstArena *arena, int line, AstNode *condition,
                          AstNode *thenValue, AstNode *elseValue) {
  AstNode *node = newNode(arena, AST_CONDITIONAL, line);
  if (node == NULL) return NULL;
  node->as.conditional.condition = condition;
  node->as.conditional.thenValue = thenValue;
  node->as.conditional.elseValue = elseValue;
  return node;
}

AstNode *csAstBreak(AstArena *arena, int line) {
  return newNode(arena, AST_BREAK_STMT, line);
}

AstNode *csAstContinue(AstArena *arena, int line) {
  return newNode(arena, AST_CONTINUE_STMT, line);
}

AstNode *csAstSwitch(AstArena *arena, int line, AstNode *subject) {
  AstNode *node = newNode(arena, AST_SWITCH_STMT, line);
  if (node == NULL) return NULL;
  node->as.switchStmt.subject = subject;
  node->as.switchStmt.cases = NULL;
  node->as.switchStmt.caseCount = 0;
  node->as.switchStmt.defaultBody = NULL;
  return node;
}

void csAstSwitchAddCase(AstArena *arena, AstNode *node, AstNode *test, AstNode *body) {
  if (node == NULL) return;
  int count = node->as.switchStmt.caseCount;
  AstSwitchCase *grown = (AstSwitchCase *)csAstArenaAlloc(
      arena, sizeof(AstSwitchCase) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (node->as.switchStmt.cases != NULL) {
    memcpy(grown, node->as.switchStmt.cases, sizeof(AstSwitchCase) * (size_t)count);
  }
  grown[count].test = test;
  grown[count].body = body;
  node->as.switchStmt.cases = grown;
  node->as.switchStmt.caseCount = count + 1;
}

AstNode *csAstIndex(AstArena *arena, int line, AstNode *target, AstNode *index) {
  AstNode *node = newNode(arena, AST_INDEX, line);
  if (node == NULL) return NULL;
  node->as.index.target = target;
  node->as.index.index = index;
  return node;
}

AstNode *csAstObjectLiteral(AstArena *arena, int line) {
  AstNode *node = newNode(arena, AST_OBJECT_LITERAL, line);
  if (node == NULL) return NULL;
  node->as.objectLiteral.keys = NULL;
  node->as.objectLiteral.values = NULL;
  node->as.objectLiteral.count = 0;
  return node;
}

void csAstObjectLiteralAdd(AstArena *arena, AstNode *object, AstNode *key,
                           AstNode *value) {
  if (object == NULL || key == NULL || value == NULL) return;
  int count = object->as.objectLiteral.count;

  AstNode **keys = growList(arena, object->as.objectLiteral.keys, count);
  AstNode **values = growList(arena, object->as.objectLiteral.values, count);
  if (keys == NULL || values == NULL) return;

  keys[count] = key;
  values[count] = value;
  object->as.objectLiteral.keys = keys;
  object->as.objectLiteral.values = values;
  object->as.objectLiteral.count = count + 1;
}

AstNode *csAstArrayLiteral(AstArena *arena, int line) {
  AstNode *node = newNode(arena, AST_ARRAY_LITERAL, line);
  if (node == NULL) return NULL;
  node->as.arrayLiteral.elements = NULL;
  node->as.arrayLiteral.count = 0;
  return node;
}

void csAstArrayLiteralAdd(AstArena *arena, AstNode *array, AstNode *element) {
  if (array == NULL || element == NULL) return;
  int count = array->as.arrayLiteral.count;
  AstNode **grown = growList(arena, array->as.arrayLiteral.elements, count);
  if (grown == NULL) return;
  grown[count] = element;
  array->as.arrayLiteral.elements = grown;
  array->as.arrayLiteral.count = count + 1;
}

AstNode *csAstForOf(AstArena *arena, int line, const char *name, int nameLength,
                    bool isConst, AstNode *iterable, AstNode *body) {
  AstNode *node = newNode(arena, AST_FOR_OF_STMT, line);
  if (node == NULL) return NULL;
  node->as.forOf.name = internName(arena, name, nameLength, &node->as.forOf.nameLength);
  node->as.forOf.isConst = isConst;
  node->as.forOf.iterable = iterable;
  node->as.forOf.body = body;
  return node;
}

AstNode *csAstReturn(AstArena *arena, int line, AstNode *value) {
  AstNode *node = newNode(arena, AST_RETURN_STMT, line);
  if (node != NULL) node->as.returnValue = value;
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

void csAstProgramAdd(AstArena *arena, AstNode *parent, AstNode *statement) {
  if (parent == NULL || statement == NULL) return;

  /* AST_PROGRAM and AST_BLOCK have the same list layout, so one appender
   * serves both. */
  AstNode *program = parent;

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
    case BINARY_EXPONENT:         return "**";
    case BINARY_EQUAL:            return "===";
    case BINARY_NOT_EQUAL:        return "!==";
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
