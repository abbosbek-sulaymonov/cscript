/* ast.h — the parse tree, and the arena it lives in.
 *
 * The AST is a compile-time-only structure: the compiler lowers it to bytecode
 * and the whole arena is then released in one call. Because of that, no AST
 * node ever holds a GC-managed object — string and identifier literals keep a
 * slice of the source and are interned later, by the compiler.
 */
#ifndef CSCRIPT_AST_H
#define CSCRIPT_AST_H

#include "cscript/common.h"

typedef enum {
  /* Expressions. */
  AST_NUMBER_LITERAL,
  AST_STRING_LITERAL,
  AST_BOOL_LITERAL,
  AST_NULL_LITERAL,
  AST_UNDEFINED_LITERAL,
  AST_UNARY,
  AST_BINARY,
  AST_LOGICAL,
  AST_GROUPING,

  /* Statements. */
  AST_EXPRESSION_STMT,
  AST_PRINT_STMT,
  AST_PROGRAM,
} AstNodeType;

typedef enum {
  UNARY_NEGATE, /* -x */
  UNARY_NOT,    /* !x */
  UNARY_TYPEOF, /* typeof x */
} UnaryOp;

typedef enum {
  BINARY_ADD, BINARY_SUBTRACT, BINARY_MULTIPLY, BINARY_DIVIDE, BINARY_MODULO,
  BINARY_EQUAL, BINARY_NOT_EQUAL,               /* ==  != — coercing */
  BINARY_STRICT_EQUAL, BINARY_STRICT_NOT_EQUAL, /* === !== — no coercion */
  BINARY_GREATER, BINARY_GREATER_EQUAL,
  BINARY_LESS, BINARY_LESS_EQUAL,
} BinaryOp;

typedef enum {
  LOGICAL_AND, /* && */
  LOGICAL_OR,  /* || */
} LogicalOp;

typedef struct AstNode AstNode;

struct AstNode {
  AstNodeType type;
  int line;
  union {
    double number;                       /* AST_NUMBER_LITERAL */
    bool boolean;                        /* AST_BOOL_LITERAL */
    struct {                             /* AST_STRING_LITERAL */
      const char *chars;                 /*   decoded, arena-owned */
      int length;
    } string;
    struct {                             /* AST_UNARY */
      UnaryOp op;
      AstNode *operand;
    } unary;
    struct {                             /* AST_BINARY */
      BinaryOp op;
      AstNode *left;
      AstNode *right;
    } binary;
    struct {                             /* AST_LOGICAL */
      LogicalOp op;
      AstNode *left;
      AstNode *right;
    } logical;
    AstNode *grouping;                   /* AST_GROUPING */
    AstNode *expression;                 /* AST_EXPRESSION_STMT, AST_PRINT_STMT */
    struct {                             /* AST_PROGRAM */
      AstNode **statements;
      int count;
      int capacity;
    } program;
  } as;
};

/* Bump allocator. Nodes are never freed individually; the arena is reset or
 * destroyed as a unit once compilation is done. */
typedef struct AstArenaBlock AstArenaBlock;

typedef struct {
  AstArenaBlock *head;
  size_t bytesAllocated;
} AstArena;

void csAstArenaInit(AstArena *arena);
void csAstArenaFree(AstArena *arena);
void *csAstArenaAlloc(AstArena *arena, size_t size);

/* Node constructors. Every one allocates from `arena`. */
AstNode *csAstNumber(AstArena *arena, int line, double value);
AstNode *csAstString(AstArena *arena, int line, const char *chars, int length);
AstNode *csAstBool(AstArena *arena, int line, bool value);
AstNode *csAstNull(AstArena *arena, int line);
AstNode *csAstUndefined(AstArena *arena, int line);
AstNode *csAstUnary(AstArena *arena, int line, UnaryOp op, AstNode *operand);
AstNode *csAstBinary(AstArena *arena, int line, BinaryOp op, AstNode *left,
                     AstNode *right);
AstNode *csAstLogical(AstArena *arena, int line, LogicalOp op, AstNode *left,
                      AstNode *right);
AstNode *csAstGrouping(AstArena *arena, int line, AstNode *inner);
AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression);
AstNode *csAstPrintStmt(AstArena *arena, int line, AstNode *expression);
AstNode *csAstProgram(AstArena *arena, int line);
void csAstProgramAdd(AstArena *arena, AstNode *program, AstNode *statement);

const char *csUnaryOpName(UnaryOp op);
const char *csBinaryOpName(BinaryOp op);
const char *csLogicalOpName(LogicalOp op);

#endif /* CSCRIPT_AST_H */
