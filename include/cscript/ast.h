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
#include "cscript/type.h"

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
  AST_IDENTIFIER,
  AST_ASSIGN,
  AST_UPDATE,
  AST_CALL,
  AST_PROPERTY,

  /* Statements. */
  AST_EXPRESSION_STMT,
  AST_VAR_DECL,
  AST_BLOCK,
  AST_IF_STMT,
  AST_WHILE_STMT,
  AST_FOR_STMT,
  AST_PROGRAM,
} AstNodeType;

typedef enum {
  UNARY_NEGATE, /* -x */
  UNARY_NOT,    /* !x */
  UNARY_TYPEOF, /* typeof x */
} UnaryOp;

typedef enum {
  BINARY_ADD, BINARY_SUBTRACT, BINARY_MULTIPLY, BINARY_DIVIDE, BINARY_MODULO,
  BINARY_EQUAL, BINARY_NOT_EQUAL, /* === !== — CScript has no coercing equality */
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

  /* Filled in by the type checker. The compiler reads it to specialise code,
   * which is why annotations are consumed rather than erased. TYPE_ANY means
   * "not known statically", not "unchecked". */
  TypeKind resolvedType;
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
    AstNode *expression;                 /* AST_EXPRESSION_STMT */
    struct {                             /* AST_IDENTIFIER */
      const char *name;                  /*   arena-owned, NUL-terminated */
      int length;
    } identifier;
    struct {                             /* AST_ASSIGN */
      AstNode *target;                   /*   an identifier for now */
      AstNode *value;
    } assign;
    struct {                             /* AST_UPDATE — ++x, x++, --x, x-- */
      AstNode *target;
      bool isIncrement;
      bool isPrefix;                     /*   prefix yields the new value */
    } update;
    struct {                             /* AST_CALL */
      AstNode *callee;
      AstNode **arguments;
      int argCount;
    } call;
    struct {                             /* AST_PROPERTY — obj.name */
      AstNode *object;
      const char *name;
      int length;
    } property;
    struct {                             /* AST_VAR_DECL */
      const char *name;
      int length;
      AstNode *initializer;              /*   NULL for `let x;` */
      bool isConst;
      TypeKind declaredType;             /*   from `: T`, else TYPE_ANY */
      bool hasAnnotation;                /*   distinguishes `: any` from none */
    } varDecl;
    struct {                             /* AST_BLOCK */
      AstNode **statements;
      int count;
      int capacity;
    } block;
    struct {                             /* AST_IF_STMT */
      AstNode *condition;
      AstNode *thenBranch;
      AstNode *elseBranch;               /*   NULL when there is no else */
    } ifStmt;
    struct {                             /* AST_WHILE_STMT */
      AstNode *condition;
      AstNode *body;
    } whileStmt;
    struct {                             /* AST_FOR_STMT — any clause may be NULL */
      AstNode *initializer;
      AstNode *condition;
      AstNode *increment;
      AstNode *body;
    } forStmt;
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
AstNode *csAstIdentifier(AstArena *arena, int line, const char *name, int length);
AstNode *csAstAssign(AstArena *arena, int line, AstNode *target, AstNode *value);
AstNode *csAstUpdate(AstArena *arena, int line, AstNode *target, bool isIncrement,
                     bool isPrefix);
AstNode *csAstCall(AstArena *arena, int line, AstNode *callee);
void csAstCallAddArgument(AstArena *arena, AstNode *call, AstNode *argument);
AstNode *csAstProperty(AstArena *arena, int line, AstNode *object, const char *name,
                       int length);

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression);
AstNode *csAstVarDecl(AstArena *arena, int line, const char *name, int length,
                      AstNode *initializer, bool isConst, TypeKind declaredType,
                      bool hasAnnotation);
AstNode *csAstBlock(AstArena *arena, int line);
AstNode *csAstIf(AstArena *arena, int line, AstNode *condition, AstNode *thenBranch,
                 AstNode *elseBranch);
AstNode *csAstWhile(AstArena *arena, int line, AstNode *condition, AstNode *body);
AstNode *csAstFor(AstArena *arena, int line, AstNode *initializer, AstNode *condition,
                  AstNode *increment, AstNode *body);
AstNode *csAstProgram(AstArena *arena, int line);

/* Appends to a AST_PROGRAM or AST_BLOCK statement list. */
void csAstProgramAdd(AstArena *arena, AstNode *parent, AstNode *statement);

const char *csUnaryOpName(UnaryOp op);
const char *csBinaryOpName(BinaryOp op);
const char *csLogicalOpName(LogicalOp op);

#endif /* CSCRIPT_AST_H */
