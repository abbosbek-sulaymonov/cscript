/* typecheck_internal.h — the checker's state, and the seam between its files.
 *
 * The scope stack mirrors the compiler's deliberately: keeping the two passes
 * independent means the compiler can be changed without silently altering what
 * is or is not an error. Nothing outside src/compiler/typecheck*.c includes
 * this.
 */
#ifndef CSCRIPT_COMPILER_TYPECHECK_INTERNAL_H
#define CSCRIPT_COMPILER_TYPECHECK_INTERNAL_H

#include "cscript/ast.h"
#include "cscript/diagnostic.h"
#include "cscript/type.h"
#include "cscript/typecheck.h"


#define MAX_SCOPED_VARIABLES 512

/* A function's declared shape. Parameter and return types are kept here rather
 * than in TypeKind, which stays a flat enum: a full type tree is only worth
 * building once object shapes and generics need one. */
typedef struct {
  TypeKind returnType;
  bool hasReturnAnnotation;
  int paramCount;
  /* How many a call must supply. A parameter with a default is optional, so
   * this stops at the first one that has one. */
  int requiredCount;
  bool hasRest; /* no upper bound on the arguments */
  TypeKind paramTypes[UINT8_MAX];
} Signature;

/* A variable the checker knows about. The scope stack mirrors the compiler's,
 * deliberately: keeping the two passes independent means the compiler can be
 * changed without silently altering what is or is not an error. */
typedef struct {
  const char *name;
  int length;
  TypeKind type;
  int depth;
  const Signature *signature; /* NULL unless the variable is a function */
} Variable;

#define MAX_FUNCTIONS 128

typedef struct {
  Diagnostics *diag;
  Variable variables[MAX_SCOPED_VARIABLES];
  int count;
  int scopeDepth;

  /* Signatures are owned here so they outlive the scope that declared them. */
  Signature signatures[MAX_FUNCTIONS];
  int signatureCount;

  /* The return type expected by the function currently being checked, so a
   * `return` can be validated against its own declaration. */
  TypeKind currentReturn;
  bool currentReturnAnnotated;
  int functionDepth;
} Checker;

/* What a built-in method on a primitive answers.
 *
 * TYPE_ANY here means "known method, unknown result" — an array method whose
 * element type the checker cannot see. */
typedef struct {
  TypeKind receiver;
  const char *name;
  TypeKind returns;
} MethodSignature;

/* The method that name refers to on that receiver type, or NULL. */
const MethodSignature *csTypeFindMethod(TypeKind receiver, const char *name,
                                        int length);

/* Requires a number, reporting against the operator that wanted one. Answers
 * TYPE_NUMBER either way, so one bad operand does not cascade. */
TypeKind csTypeRequireNumber(Checker *checker, TypeKind type, int line,
                             const char *what);

void csTypeBeginScope(Checker *checker);
void csTypeDeclareVariable(Checker *checker, const char *name, int length,
                           TypeKind type);

/* Reports at a node's line, and counts it. */
void csTypeError(Checker *checker, int line, const char *format, ...);

/* Drops every variable the innermost scope declared. */
void csTypeEndScope(Checker *checker);

/* The variable that name refers to here, or NULL. Innermost first, so a
 * shadowing declaration wins. */
Variable *csTypeFindVariable(Checker *checker, const char *name, int length);

/* The type of a node, and the annotation left on it. */
TypeKind checkNode(Checker *checker, AstNode *node);

/* What a binary operator's operands have to be, and what it answers. */
TypeKind csTypeCheckBinary(Checker *checker, AstNode *node);

/* Records a function's shape and binds its name, before the body is walked so
 * that recursive calls resolve. */
const Signature *csTypeDeclareFunction(Checker *checker, AstNode *node);

/* Walks a function's body with its own return type in scope. */
void csTypeCheckFunctionBody(Checker *checker, AstNode *node,
                             const Signature *signature);

/* The two halves checkNode asks, in order. Each answers false for a node
 * belonging to the other. */
bool checkValueNode(Checker *checker, AstNode *node, TypeKind *result);
bool checkStatementNode(Checker *checker, AstNode *node, TypeKind *result);

#endif /* CSCRIPT_COMPILER_TYPECHECK_INTERNAL_H */
