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

void csTypeBeginScope(Checker *checker) {
  checker->scopeDepth++;
}

void csTypeEndScope(Checker *checker) {
  checker->scopeDepth--;
  while (checker->count > 0 && checker->variables[checker->count - 1].depth > checker->scopeDepth) {
    checker->count--;
  }
}

/* Searches innermost-first so a shadowing declaration wins. */
Variable *csTypeFindVariable(Checker *checker, const char *name, int length) {
  for (int i = checker->count - 1; i >= 0; i--) {
    Variable *variable = &checker->variables[i];
    if (variable->length == length && memcmp(variable->name, name, (size_t)length) == 0) {
      return variable;
    }
  }
  return NULL;
}

void csTypeDeclareVariable(Checker *checker, const char *name, int length, TypeId type) {
  if (checker->count >= MAX_SCOPED_VARIABLES) return; /* compiler reports the limit */
  Variable *variable = &checker->variables[checker->count++];
  variable->name = name;
  variable->length = length;
  variable->type = type;
  variable->depth = checker->scopeDepth;
  variable->signature = NULL;
  variable->awaiting = false;
}

/* `let x;` — nothing to infer from yet. It holds undefined until something is
 * assigned, and the type of that first assignment is the type it keeps. */
void csTypeDeclareAwaiting(Checker *checker, const char *name, int length) {
  csTypeDeclareVariable(checker, name, length, TYPE_UNDEFINED);
  if (checker->count > 0) checker->variables[checker->count - 1].awaiting = true;
}

/* The built-in globals, so `console.log(...)` and `Math.PI` check out. */
static void declareBuiltins(Checker *checker) {
  static const struct {
    const char *name;
    TypeId type;
  } builtins[] = {
      {"console", TYPE_OBJECT},   {"Math", TYPE_OBJECT}, {"Number", TYPE_FUNCTION}, {"String", TYPE_FUNCTION},
      {"Boolean", TYPE_FUNCTION}, {"NaN", TYPE_NUMBER},  {"Infinity", TYPE_NUMBER}, {"Error", TYPE_FUNCTION},
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    csTypeDeclareVariable(checker, builtins[i].name, (int)strlen(builtins[i].name), builtins[i].type);
  }
}

/* What the built-in methods return, so `"a".toUpperCase()` is a string rather
 * than dynamic. Only the return type is modelled; argument checking is left to
 * the runtime, because several of these are variadic or accept several shapes.
 *
 * TYPE_DYNAMIC here means "known method, unknown result" — an array method whose
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
    {TYPE_STRING, "match", TYPE_DYNAMIC},
    {TYPE_STRING, "search", TYPE_NUMBER},
    {TYPE_STRING, "split", TYPE_ARRAY},
    {TYPE_STRING, "padStart", TYPE_STRING},
    {TYPE_STRING, "padEnd", TYPE_STRING},
    {TYPE_STRING, "concat", TYPE_STRING},
    {TYPE_STRING, "at", TYPE_DYNAMIC}, /* undefined past either end */

    /* arrays. The element type is still unknown, so what a method answers is
     * as far as the table goes: `map` gives an array, `join` a string. */
    {TYPE_ARRAY, "push", TYPE_NUMBER},
    {TYPE_ARRAY, "unshift", TYPE_NUMBER},
    {TYPE_ARRAY, "indexOf", TYPE_NUMBER},
    {TYPE_ARRAY, "lastIndexOf", TYPE_NUMBER},
    {TYPE_ARRAY, "includes", TYPE_BOOLEAN},
    {TYPE_ARRAY, "some", TYPE_BOOLEAN},
    {TYPE_ARRAY, "every", TYPE_BOOLEAN},
    {TYPE_ARRAY, "findIndex", TYPE_NUMBER},
    {TYPE_ARRAY, "join", TYPE_STRING},
    {TYPE_ARRAY, "slice", TYPE_ARRAY},
    {TYPE_ARRAY, "concat", TYPE_ARRAY},
    {TYPE_ARRAY, "map", TYPE_ARRAY},
    {TYPE_ARRAY, "filter", TYPE_ARRAY},
    {TYPE_ARRAY, "reverse", TYPE_ARRAY},
    {TYPE_ARRAY, "fill", TYPE_ARRAY},
    {TYPE_ARRAY, "sort", TYPE_ARRAY},
    {TYPE_ARRAY, "forEach", TYPE_UNDEFINED},
    {TYPE_ARRAY, "at", TYPE_DYNAMIC},
    {TYPE_ARRAY, "flat", TYPE_ARRAY},
    {TYPE_ARRAY, "flatMap", TYPE_ARRAY},

    /* numbers */
    {TYPE_NUMBER, "toFixed", TYPE_STRING},
    {TYPE_NUMBER, "toPrecision", TYPE_STRING},
    {TYPE_NUMBER, "toString", TYPE_STRING},

    {TYPE_BIGINT, "toString", TYPE_STRING},
    {TYPE_BIGINT, "toLocaleString", TYPE_STRING},
    {TYPE_BIGINT, "valueOf", TYPE_BIGINT},
};

/* Returns the signature for `name` on `receiver`, or NULL. */
const MethodSignature *csTypeFindMethod(TypeId receiver, const char *name, int length) {
  for (size_t i = 0; i < sizeof(BUILTIN_METHODS) / sizeof(BUILTIN_METHODS[0]); i++) {
    const MethodSignature *entry = &BUILTIN_METHODS[i];
    if (entry->receiver != receiver) continue;
    if ((int)strlen(entry->name) == length && memcmp(entry->name, name, (size_t)length) == 0) {
      return entry;
    }
  }
  return NULL;
}

/* Records a function's shape and binds its name, before the body is walked so
 * that recursive calls resolve. */
Signature *csTypeDeclareFunction(Checker *checker, AstNode *node) {
  if (checker->signatureCount >= MAX_FUNCTIONS) return NULL;

  Signature *signature = &checker->signatures[checker->signatureCount++];
  signature->returnType = node->as.function.hasReturnAnnotation ? node->as.function.returnType : TYPE_DYNAMIC;
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
    signature->paramTypes[i] = param->hasAnnotation ? param->type : TYPE_DYNAMIC;
    signature->paramHasDefault[i] = param->defaultValue != NULL;

    /* A declaration takes its type from what it is given; a parameter has
     * nothing to take one from, so it has to say. Without this rule every
     * unannotated parameter would be the escape hatch the language is built
     * on not having — an `any` with its name taken away.
     *
     * A destructured parameter is exempt: the pattern names the parts, and
     * annotating the whole would say less than the pattern already does. */
    if (param->hasAnnotation || param->pattern != NULL) continue;
    csTypeError(checker, node->line,
                "parameter '%.*s' needs a type: a parameter has no initialiser "
                "to take one from",
                param->length, param->name);
  }

  /* The same shape, written as a type: `(number, string) => boolean`. A
   * declaration and an annotation then say the same thing in the same
   * language, which is what lets a named function be passed where a
   * `(n: number) => string` is wanted and be checked for it. */
  signature->type = csTypeFunctionOf(checker->types, signature->paramTypes, signature->paramCount, signature->requiredCount, signature->hasRest, signature->returnType);

  /* `function first<T>(xs: T[]): T` — the variables it introduced, kept so
   * that a call can work out what they stand for *there*. */
  signature->typeParamCount = node->as.function.typeParamCount;
  for (int i = 0; i < signature->typeParamCount; i++) signature->typeParams[i] = node->as.function.typeParams[i];

  if (node->as.function.isDeclaration) {
    csTypeDeclareVariable(checker, node->as.function.name, node->as.function.nameLength, signature->type);
    checker->variables[checker->count - 1].signature = signature;
  }
  return signature;
}

void csTypeCheckFunctionBody(Checker *checker, AstNode *node, Signature *signature) {
  TypeId savedReturn = checker->currentReturn;
  bool savedAnnotated = checker->currentReturnAnnotated;
  TypeId savedInferred = checker->inferredReturn;
  checker->currentReturn = signature != NULL ? signature->returnType : TYPE_DYNAMIC;
  checker->currentReturnAnnotated = signature != NULL && signature->hasReturnAnnotation;

  /* `async function f(): Promise<number>` returns a *number*: what the
   * annotation describes is what calling it answers, and a `return` inside
   * gives what that promise resolves to. Both spellings work — an async
   * function annotated `number` says the same thing about its returns — so the
   * two readings agree on everything but which one the caller awaits. */
  if (node->as.function.isAsync && checker->currentReturnAnnotated) {
    TypeId resolves;
    if (csTypeIsPromise(checker->types, checker->currentReturn, &resolves)) checker->currentReturn = resolves;
  }
  checker->inferredReturn = TYPE_ERROR;
  checker->functionDepth++;

  csTypeBeginScope(checker);
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    TypeId type = param->hasAnnotation ? param->type : TYPE_DYNAMIC;

    /* `...rest: string` annotates each argument, not the collection: the
     * caller passes strings and the body reads an array of them. Annotating
     * the array instead would say nothing about what is in it, which is the
     * half a caller needs checked. */
    bool isRest = node->as.function.hasRest && i == node->as.function.paramCount - 1;
    csTypeDeclareVariable(checker, param->name, param->length, isRest ? TYPE_ARRAY : type);
  }
  /* The body is an AST_BLOCK, but its statements are checked in the scope that
   * already holds the parameters rather than in one nested inside it. */
  for (int i = 0; i < node->as.function.body->as.block.count; i++) {
    checkNode(checker, node->as.function.body->as.block.statements[i]);
  }
  csTypeEndScope(checker);

  checker->functionDepth--;

  /* A function that did not say what it answers is asked. Its `return`
   * statements have been collecting a type on the way through, and taking it
   * is what lets `const double = (n: number) => n * 2` be passed where a
   * `(n: number) => number` is wanted — the commonest callback there is.
   *
   * Left alone when a body returns nothing the checker could name, because
   * DYNAMIC there means "ask the runtime" rather than "answers undefined". */
  /* A generator answers a generator and an async function answers a promise,
   * neither of which is what its `return` says — so neither is inferred.
   * Writing the annotation on one of those is already understood as what it
   * resolves to, which is the same distinction. */
  bool inferable = !node->as.function.isAsync && !node->as.function.isGenerator;
  if (signature != NULL && inferable && !signature->hasReturnAnnotation && checker->inferredReturn != TYPE_ERROR) {
    signature->returnType = checker->inferredReturn;
    signature->type =
        csTypeFunctionOf(checker->types, signature->paramTypes, signature->paramCount, signature->requiredCount, signature->hasRest, signature->returnType);

    /* The name was bound before the body was walked, so that the function
     * could call itself; the binding is brought up to date here. */
    if (node->as.function.isDeclaration) {
      Variable *variable = csTypeFindVariable(checker, node->as.function.name, node->as.function.nameLength);
      if (variable != NULL && variable->signature == signature) variable->type = signature->type;
    }
  }

  checker->currentReturn = savedReturn;
  checker->currentReturnAnnotated = savedAnnotated;
  checker->inferredReturn = savedInferred;
}

/* Arithmetic where a BigInt is allowed, so long as *both* sides are one.
 *
 * This is the rule BigInt exists for: `1n + 1` has no answer that is both a
 * BigInt and a number, so JavaScript throws rather than choose, and the checker
 * says so at compile time. Returns the result type, or TYPE_DYNAMIC when it cannot
 * tell yet and the VM must decide. */
static bool arithmeticOnBigInts(Checker *checker, TypeId left, TypeId right, int line, const char *name, TypeId *result) {
  if (left != TYPE_BIGINT && right != TYPE_BIGINT) return false;

  if (left == TYPE_BIGINT && right == TYPE_BIGINT) {
    *result = TYPE_BIGINT;
    return true;
  }
  /* `any` on the other side could still be a BigInt at run time. */
  if (left == TYPE_DYNAMIC || right == TYPE_DYNAMIC || left == TYPE_ERROR || right == TYPE_ERROR) {
    *result = TYPE_DYNAMIC;
    return true;
  }

  csTypeError(checker, line, "cannot mix BigInt and %s in '%s'", csTypeNameIn(checker->types, left == TYPE_BIGINT ? right : left), name);
  *result = TYPE_ERROR;
  return true;
}

/* Requires a number, reporting against the operator that wanted one. */
/* A `value` has to be narrowed before it can be used for anything. One message
 * for every place that happens, because the fix is always the same. */
bool csTypeRefuseUnnarrowed(Checker *checker, TypeId type, int line, const char *what, AstNode *subject) {
  if (type != TYPE_UNKNOWN) return false;

  /* Naming the thing that was not narrowed is most of the fix: the message has
   * to say which of several operands the checker is complaining about, and the
   * example then reads as something the programmer can paste. */
  if (subject != NULL && subject->type == AST_IDENTIFIER) {
    csTypeError(checker, line,
                "%s needs to know what '%.*s' is: narrow it first, as in "
                "`if (typeof %.*s === \"number\")`",
                what, subject->as.identifier.length, subject->as.identifier.name, subject->as.identifier.length, subject->as.identifier.name);
    return true;
  }
  csTypeError(checker, line,
              "%s needs to know what this is: check it with typeof first, as in "
              "`if (typeof x === \"number\")`",
              what);
  return true;
}

TypeId csTypeRequireNumber(Checker *checker, TypeId type, int line, const char *operatorName, AstNode *subject) {
  if (type == TYPE_UNKNOWN) {
    char what[64];
    snprintf(what, sizeof what, "the operand of '%s'", operatorName);
    csTypeRefuseUnnarrowed(checker, type, line, what, subject);
    return TYPE_ERROR;
  }
  if (csTypeAssignableIn(checker->types, type, TYPE_NUMBER)) return TYPE_NUMBER;
  csTypeError(checker, line, "operand of '%s' must be a number, got %s", operatorName, csTypeNameIn(checker->types, type));
  return TYPE_ERROR;
}

TypeId csTypeCheckBinary(Checker *checker, AstNode *node) {
  TypeId left = checkNode(checker, node->as.binary.left);
  TypeId right = checkNode(checker, node->as.binary.right);
  int line = node->line;
  const char *name = csBinaryOpName(node->as.binary.op);

  switch (node->as.binary.op) {
    case BINARY_ADD:
      /* `+` is the one operator that stays polymorphic: a string on either side
       * makes it concatenation. */
      if (left == TYPE_ERROR || right == TYPE_ERROR) return TYPE_ERROR;

      /* A `value` has no `+`: it might be a number, a string, or an object
       * with neither. Saying so in the narrowing language keeps one answer for
       * the one question — what is this? — rather than two for two operators. */
      if (csTypeRefuseUnnarrowed(checker, left, line, "adding", node->as.binary.left) ||
          csTypeRefuseUnnarrowed(checker, right, line, "adding", node->as.binary.right)) {
        return TYPE_ERROR;
      }
      if (left == TYPE_STRING || right == TYPE_STRING) return TYPE_STRING;
      if (left == TYPE_BIGINT || right == TYPE_BIGINT) {
        TypeId result;
        arithmeticOnBigInts(checker, left, right, line, name, &result);
        return result;
      }
      if (left == TYPE_DYNAMIC || right == TYPE_DYNAMIC) return TYPE_DYNAMIC;
      if (left == TYPE_NUMBER && right == TYPE_NUMBER) return TYPE_NUMBER;
      csTypeError(checker, line, "cannot add %s and %s", csTypeNameIn(checker->types, left), csTypeNameIn(checker->types, right));
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
      TypeId onBigInts;
      if (arithmeticOnBigInts(checker, left, right, line, name, &onBigInts)) {
        return onBigInts;
      }
      TypeId a = csTypeRequireNumber(checker, left, line, name, node->as.binary.left);
      TypeId b = csTypeRequireNumber(checker, right, line, name, node->as.binary.right);
      return (a == TYPE_ERROR || b == TYPE_ERROR) ? TYPE_ERROR : TYPE_NUMBER;
    }

    case BINARY_GREATER:
    case BINARY_GREATER_EQUAL:
    case BINARY_LESS:
    case BINARY_LESS_EQUAL:
      /* Ordering is the one place a BigInt and a number mix freely: there is
       * always an answer, and the VM compares them exactly rather than by
       * rounding the BigInt to a double. */
      if (left != TYPE_BIGINT) {
        csTypeRequireNumber(checker, left, line, name, node->as.binary.left);
      }
      if (right != TYPE_BIGINT) {
        csTypeRequireNumber(checker, right, line, name, node->as.binary.right);
      }
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
      bool involvesNullish = left == TYPE_NULL || right == TYPE_NULL || left == TYPE_UNDEFINED || right == TYPE_UNDEFINED;

      /* A `value` is exempt as well, and for the same reason the other way
       * round: asking whether one equals a number is how a program finds out
       * what it holds. `===` is the one place a `value` needs no narrowing —
       * the question is what narrowing would answer. */
      bool involvesValue = left == TYPE_UNKNOWN || right == TYPE_UNKNOWN;

      if (!involvesNullish && !involvesValue && csTypeIsKnown(left) && csTypeIsKnown(right) && left != right) {
        csTypeError(checker, line, "'%s' between %s and %s is always %s — the types can never match", name, csTypeNameIn(checker->types, left),
                    csTypeNameIn(checker->types, right), node->as.binary.op == BINARY_EQUAL ? "false" : "true");
        return TYPE_ERROR;
      }
      return TYPE_BOOLEAN;
    }
  }
  return TYPE_DYNAMIC;
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
TypeId checkNode(Checker *checker, AstNode *node) {
  if (node == NULL) return TYPE_DYNAMIC;

  TypeId result = TYPE_DYNAMIC;
  if (!checkValueNode(checker, node, &result)) {
    checkStatementNode(checker, node, &result);
  }

  node->resolvedType = result;
  return result;
}

bool csTypeCheck(AstNode *program, Diagnostics *diag, const char *sourcePath) {
  Checker checker;
  checker.diag = diag;
  checker.count = 0;
  checker.scopeDepth = 0;
  checker.signatureCount = 0;
  checker.currentReturn = TYPE_DYNAMIC;
  checker.currentReturnAnnotated = false;
  checker.inferredReturn = TYPE_ERROR;
  checker.functionDepth = 0;
  checker.types = program != NULL && program->type == AST_PROGRAM ? program->as.program.types : NULL;
  checker.path = sourcePath;
  checker.program = program;

  declareBuiltins(&checker);
  checkNode(&checker, program);

  return !csDiagnosticsFailed(diag);
}
