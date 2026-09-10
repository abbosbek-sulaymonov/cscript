/* typecheck.c — static checking, between the parser and the compiler.
 *
 * It needs the whole tree, and the compiler benefits from the types it
 * resolves: every node is annotated on the way out, and `resolvedType` is what
 * lets the compiler emit OP_ADD_NUM where it would otherwise emit OP_ADD. So
 * an annotation is consumed rather than erased.
 *
 * The scope, the builtins and the signatures are here; what each node means is
 * in typecheck_value.c and typecheck_statement.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#include "compiler/typecheck_internal.h"


void csTypeError(Checker *checker, int line, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  /* Type errors are independent of one another, unlike syntax errors, so each
   * one is worth reporting. Clearing panic mode keeps them all visible. */
  checker->diag->panicMode = false;
  csDiagnosticError(checker->diag, line, NULL, 0, "%s", message);
}

void csTypeBeginScope(Checker *checker) { checker->scopeDepth++; }

void csTypeEndScope(Checker *checker) {
  checker->scopeDepth--;
  while (checker->count > 0 &&
         checker->variables[checker->count - 1].depth > checker->scopeDepth) {
    checker->count--;
  }
}

/* Searches innermost-first so a shadowing declaration wins. */
Variable *csTypeFindVariable(Checker *checker, const char *name, int length) {
  for (int i = checker->count - 1; i >= 0; i--) {
    Variable *variable = &checker->variables[i];
    if (variable->length == length &&
        memcmp(variable->name, name, (size_t)length) == 0) {
      return variable;
    }
  }
  return NULL;
}

void csTypeDeclareVariable(Checker *checker, const char *name, int length,
                            TypeKind type) {
  if (checker->count >= MAX_SCOPED_VARIABLES) return; /* compiler reports the limit */
  Variable *variable = &checker->variables[checker->count++];
  variable->name = name;
  variable->length = length;
  variable->type = type;
  variable->depth = checker->scopeDepth;
  variable->signature = NULL;
}

/* The built-in globals, so `console.log(...)` and `Math.PI` check out. */
static void declareBuiltins(Checker *checker) {
  static const struct {
    const char *name;
    TypeKind type;
  } builtins[] = {
      {"console", TYPE_OBJECT}, {"Math", TYPE_OBJECT},   {"Number", TYPE_FUNCTION},
      {"String", TYPE_FUNCTION}, {"Boolean", TYPE_FUNCTION}, {"NaN", TYPE_NUMBER},
      {"Infinity", TYPE_NUMBER}, {"Error", TYPE_FUNCTION},
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    csTypeDeclareVariable(checker, builtins[i].name, (int)strlen(builtins[i].name),
                    builtins[i].type);
  }
}

/* What the built-in methods return, so `"a".toUpperCase()` is a string rather
 * than dynamic. Only the return type is modelled; argument checking is left to
 * the runtime, because several of these are variadic or accept several shapes.
 *
 * TYPE_ANY here means "known method, unknown result" — an array method whose
 * element type the checker cannot see. */
static const MethodSignature BUILTIN_METHODS[] = {
    /* strings */
    {TYPE_STRING, "toUpperCase", TYPE_STRING},
    {TYPE_STRING, "toLowerCase", TYPE_STRING},
    {TYPE_STRING, "trim", TYPE_STRING},
    {TYPE_STRING, "trimStart", TYPE_STRING},
    {TYPE_STRING, "trimEnd", TYPE_STRING},
    {TYPE_STRING, "slice", TYPE_STRING},
    {TYPE_STRING, "substring", TYPE_STRING},
    {TYPE_STRING, "charAt", TYPE_STRING},
    {TYPE_STRING, "charCodeAt", TYPE_NUMBER},
    {TYPE_STRING, "indexOf", TYPE_NUMBER},
    {TYPE_STRING, "lastIndexOf", TYPE_NUMBER},
    {TYPE_STRING, "includes", TYPE_BOOLEAN},
    {TYPE_STRING, "startsWith", TYPE_BOOLEAN},
    {TYPE_STRING, "endsWith", TYPE_BOOLEAN},
    {TYPE_STRING, "repeat", TYPE_STRING},
    {TYPE_STRING, "replace", TYPE_STRING},
    {TYPE_STRING, "replaceAll", TYPE_STRING},
    /*  gives an array or null, and  an index — both dynamic
     * enough that the checker only records that the name exists. */
    {TYPE_STRING, "match", TYPE_ANY},
    {TYPE_STRING, "search", TYPE_NUMBER},
    {TYPE_STRING, "split", TYPE_OBJECT},
    {TYPE_STRING, "padStart", TYPE_STRING},
    {TYPE_STRING, "padEnd", TYPE_STRING},
    {TYPE_STRING, "concat", TYPE_STRING},
    {TYPE_STRING, "at", TYPE_ANY}, /* undefined past either end */

    /* arrays — the receiver is TYPE_OBJECT until element types exist */
    {TYPE_OBJECT, "push", TYPE_NUMBER},
    {TYPE_OBJECT, "unshift", TYPE_NUMBER},
    {TYPE_OBJECT, "indexOf", TYPE_NUMBER},
    {TYPE_OBJECT, "lastIndexOf", TYPE_NUMBER},
    {TYPE_OBJECT, "includes", TYPE_BOOLEAN},
    {TYPE_OBJECT, "some", TYPE_BOOLEAN},
    {TYPE_OBJECT, "every", TYPE_BOOLEAN},
    {TYPE_OBJECT, "findIndex", TYPE_NUMBER},
    {TYPE_OBJECT, "join", TYPE_STRING},
    {TYPE_OBJECT, "slice", TYPE_OBJECT},
    {TYPE_OBJECT, "concat", TYPE_OBJECT},
    {TYPE_OBJECT, "map", TYPE_OBJECT},
    {TYPE_OBJECT, "filter", TYPE_OBJECT},
    {TYPE_OBJECT, "reverse", TYPE_OBJECT},
    {TYPE_OBJECT, "fill", TYPE_OBJECT},
    {TYPE_OBJECT, "sort", TYPE_OBJECT},
    {TYPE_OBJECT, "forEach", TYPE_UNDEFINED},
    {TYPE_OBJECT, "at", TYPE_ANY},
    {TYPE_OBJECT, "flat", TYPE_OBJECT},
    {TYPE_OBJECT, "flatMap", TYPE_OBJECT},

    /* numbers */
    {TYPE_NUMBER, "toFixed", TYPE_STRING},
    {TYPE_NUMBER, "toPrecision", TYPE_STRING},
    {TYPE_NUMBER, "toString", TYPE_STRING},

    {TYPE_BIGINT, "toString", TYPE_STRING},
    {TYPE_BIGINT, "toLocaleString", TYPE_STRING},
    {TYPE_BIGINT, "valueOf", TYPE_BIGINT},
};

/* Returns the signature for `name` on `receiver`, or NULL. */
const MethodSignature *csTypeFindMethod(TypeKind receiver, const char *name,
                                         int length) {
  for (size_t i = 0; i < sizeof(BUILTIN_METHODS) / sizeof(BUILTIN_METHODS[0]); i++) {
    const MethodSignature *entry = &BUILTIN_METHODS[i];
    if (entry->receiver != receiver) continue;
    if ((int)strlen(entry->name) == length &&
        memcmp(entry->name, name, (size_t)length) == 0) {
      return entry;
    }
  }
  return NULL;
}


/* Records a function's shape and binds its name, before the body is walked so
 * that recursive calls resolve. */
const Signature *csTypeDeclareFunction(Checker *checker, AstNode *node) {
  if (checker->signatureCount >= MAX_FUNCTIONS) return NULL;

  Signature *signature = &checker->signatures[checker->signatureCount++];
  signature->returnType =
      node->as.function.hasReturnAnnotation ? node->as.function.returnType : TYPE_ANY;
  signature->hasReturnAnnotation = node->as.function.hasReturnAnnotation;
  signature->paramCount = node->as.function.paramCount;
  signature->hasRest = node->as.function.hasRest;
  signature->requiredCount = node->as.function.paramCount;
  if (node->as.function.hasRest) signature->requiredCount--;
  for (int i = 0; i < node->as.function.paramCount; i++) {
    if (node->as.function.params[i].defaultValue != NULL) {
      signature->requiredCount = i;
      break;
    }
  }
  for (int i = 0; i < node->as.function.paramCount && i < UINT8_MAX; i++) {
    const AstParam *param = &node->as.function.params[i];
    signature->paramTypes[i] = param->hasAnnotation ? param->type : TYPE_ANY;
  }

  if (node->as.function.isDeclaration) {
    csTypeDeclareVariable(checker, node->as.function.name, node->as.function.nameLength,
                    TYPE_FUNCTION);
    checker->variables[checker->count - 1].signature = signature;
  }
  return signature;
}

void csTypeCheckFunctionBody(Checker *checker, AstNode *node,
                              const Signature *signature) {
  TypeKind savedReturn = checker->currentReturn;
  bool savedAnnotated = checker->currentReturnAnnotated;
  checker->currentReturn = signature != NULL ? signature->returnType : TYPE_ANY;
  checker->currentReturnAnnotated =
      signature != NULL && signature->hasReturnAnnotation;
  checker->functionDepth++;

  csTypeBeginScope(checker);
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    csTypeDeclareVariable(checker, param->name, param->length,
                    param->hasAnnotation ? param->type : TYPE_ANY);
  }
  /* The body is an AST_BLOCK, but its statements are checked in the scope that
   * already holds the parameters rather than in one nested inside it. */
  for (int i = 0; i < node->as.function.body->as.block.count; i++) {
    checkNode(checker, node->as.function.body->as.block.statements[i]);
  }
  csTypeEndScope(checker);

  checker->functionDepth--;
  checker->currentReturn = savedReturn;
  checker->currentReturnAnnotated = savedAnnotated;
}

/* Arithmetic where a BigInt is allowed, so long as *both* sides are one.
 *
 * This is the rule BigInt exists for: `1n + 1` has no answer that is both a
 * BigInt and a number, so JavaScript throws rather than choose, and the checker
 * says so at compile time. Returns the result type, or TYPE_ANY when it cannot
 * tell yet and the VM must decide. */
static bool arithmeticOnBigInts(Checker *checker, TypeKind left, TypeKind right,
                                int line, const char *name, TypeKind *result) {
  if (left != TYPE_BIGINT && right != TYPE_BIGINT) return false;

  if (left == TYPE_BIGINT && right == TYPE_BIGINT) {
    *result = TYPE_BIGINT;
    return true;
  }
  /* `any` on the other side could still be a BigInt at run time. */
  if (left == TYPE_ANY || right == TYPE_ANY || left == TYPE_ERROR ||
      right == TYPE_ERROR) {
    *result = TYPE_ANY;
    return true;
  }

  csTypeError(checker, line, "cannot mix BigInt and %s in '%s'",
            csTypeName(left == TYPE_BIGINT ? right : left), name);
  *result = TYPE_ERROR;
  return true;
}

/* Requires a number, reporting against the operator that wanted one. */
TypeKind csTypeRequireNumber(Checker *checker, TypeKind type, int line,
                              const char *operatorName) {
  if (csTypeAssignable(type, TYPE_NUMBER)) return TYPE_NUMBER;
  csTypeError(checker, line, "operand of '%s' must be a number, got %s", operatorName,
            csTypeName(type));
  return TYPE_ERROR;
}

TypeKind csTypeCheckBinary(Checker *checker, AstNode *node) {
  TypeKind left = checkNode(checker, node->as.binary.left);
  TypeKind right = checkNode(checker, node->as.binary.right);
  int line = node->line;
  const char *name = csBinaryOpName(node->as.binary.op);

  switch (node->as.binary.op) {
    case BINARY_ADD:
      /* `+` is the one operator that stays polymorphic: a string on either side
       * makes it concatenation. */
      if (left == TYPE_ERROR || right == TYPE_ERROR) return TYPE_ERROR;
      if (left == TYPE_STRING || right == TYPE_STRING) return TYPE_STRING;
      if (left == TYPE_BIGINT || right == TYPE_BIGINT) {
        TypeKind result;
        arithmeticOnBigInts(checker, left, right, line, name, &result);
        return result;
      }
      if (left == TYPE_ANY || right == TYPE_ANY) return TYPE_ANY;
      if (left == TYPE_NUMBER && right == TYPE_NUMBER) return TYPE_NUMBER;
      csTypeError(checker, line, "cannot add %s and %s", csTypeName(left),
                csTypeName(right));
      return TYPE_ERROR;

    case BINARY_INSTANCEOF:
      /* The right side has to be a class, which the checker cannot see, so the
       * VM does that test. The answer is a boolean either way. */
      checkNode(checker, node->as.binary.left);
      return TYPE_BOOLEAN;

    case BINARY_IN:
      /* Which keys a value has is not in the type lattice, so only the shape
       * of the operands is checked, by the VM. */
      return TYPE_BOOLEAN;

    case BINARY_SUBTRACT:
    case BINARY_MULTIPLY:
    case BINARY_DIVIDE:
    case BINARY_MODULO:
    case BINARY_EXPONENT: {
      TypeKind onBigInts;
      if (arithmeticOnBigInts(checker, left, right, line, name, &onBigInts)) {
        return onBigInts;
      }
      TypeKind a = csTypeRequireNumber(checker, left, line, name);
      TypeKind b = csTypeRequireNumber(checker, right, line, name);
      return (a == TYPE_ERROR || b == TYPE_ERROR) ? TYPE_ERROR : TYPE_NUMBER;
    }

    case BINARY_GREATER:
    case BINARY_GREATER_EQUAL:
    case BINARY_LESS:
    case BINARY_LESS_EQUAL:
      /* Ordering is the one place a BigInt and a number mix freely: there is
       * always an answer, and the VM compares them exactly rather than by
       * rounding the BigInt to a double. */
      if (left != TYPE_BIGINT) csTypeRequireNumber(checker, left, line, name);
      if (right != TYPE_BIGINT) csTypeRequireNumber(checker, right, line, name);
      return TYPE_BOOLEAN;

    case BINARY_EQUAL:
    case BINARY_NOT_EQUAL: {
      /* `===` between two known, different types can only ever be false, which
       * is a mistake worth naming rather than a value worth computing.
       *
       * Comparing against null or undefined is exempt. Those checks are
       * idiomatic, and without union types there is no way to write the type
       * of a variable that is "a string, or null" — so rejecting them would
       * punish correct code for a gap in the type system. */
      bool involvesNullish = left == TYPE_NULL || right == TYPE_NULL ||
                             left == TYPE_UNDEFINED || right == TYPE_UNDEFINED;

      if (!involvesNullish && csTypeIsKnown(left) && csTypeIsKnown(right) &&
          left != right) {
        csTypeError(checker, line,
                  "'%s' between %s and %s is always %s — the types can never match",
                  name, csTypeName(left), csTypeName(right),
                  node->as.binary.op == BINARY_EQUAL ? "false" : "true");
        return TYPE_ERROR;
      }
      return TYPE_BOOLEAN;
    }
  }
  return TYPE_ANY;
}

/* The type of a node, and the annotation left on it.
 *
 * Two halves rather than one switch, split the way the language already
 * splits: an expression has a type and a statement does not. Asked in that
 * order because expressions are the common case, and a node neither claims
 * keeps the `any` this has always fallen back to.
 *
 * The annotation is written here rather than in the halves, so that no case
 * can forget it. */
TypeKind checkNode(Checker *checker, AstNode *node) {
  if (node == NULL) return TYPE_ANY;

  TypeKind result = TYPE_ANY;
  if (!checkValueNode(checker, node, &result)) {
    checkStatementNode(checker, node, &result);
  }

  node->resolvedType = result;
  return result;
}

bool csTypeCheck(AstNode *program, Diagnostics *diag) {
  Checker checker;
  checker.diag = diag;
  checker.count = 0;
  checker.scopeDepth = 0;
  checker.signatureCount = 0;
  checker.currentReturn = TYPE_ANY;
  checker.currentReturnAnnotated = false;
  checker.functionDepth = 0;

  declareBuiltins(&checker);
  checkNode(&checker, program);

  return !csDiagnosticsFailed(diag);
}
