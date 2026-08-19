#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#define MAX_SCOPED_VARIABLES 512

/* A variable the checker knows about. The scope stack mirrors the compiler's,
 * deliberately: keeping the two passes independent means the compiler can be
 * changed without silently altering what is or is not an error. */
typedef struct {
  const char *name;
  int length;
  TypeKind type;
  int depth;
} Variable;

typedef struct {
  Diagnostics *diag;
  Variable variables[MAX_SCOPED_VARIABLES];
  int count;
  int scopeDepth;
} Checker;

static void typeError(Checker *checker, int line, const char *format, ...) {
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

static void beginScope(Checker *checker) { checker->scopeDepth++; }

static void endScope(Checker *checker) {
  checker->scopeDepth--;
  while (checker->count > 0 &&
         checker->variables[checker->count - 1].depth > checker->scopeDepth) {
    checker->count--;
  }
}

/* Searches innermost-first so a shadowing declaration wins. */
static Variable *findVariable(Checker *checker, const char *name, int length) {
  for (int i = checker->count - 1; i >= 0; i--) {
    Variable *variable = &checker->variables[i];
    if (variable->length == length &&
        memcmp(variable->name, name, (size_t)length) == 0) {
      return variable;
    }
  }
  return NULL;
}

static void declareVariable(Checker *checker, const char *name, int length,
                            TypeKind type) {
  if (checker->count >= MAX_SCOPED_VARIABLES) return; /* compiler reports the limit */
  Variable *variable = &checker->variables[checker->count++];
  variable->name = name;
  variable->length = length;
  variable->type = type;
  variable->depth = checker->scopeDepth;
}

/* The built-in globals, so `console.log(...)` and `Math.PI` check out. */
static void declareBuiltins(Checker *checker) {
  static const struct {
    const char *name;
    TypeKind type;
  } builtins[] = {
      {"console", TYPE_OBJECT}, {"Math", TYPE_OBJECT},   {"Number", TYPE_FUNCTION},
      {"String", TYPE_FUNCTION}, {"Boolean", TYPE_FUNCTION}, {"NaN", TYPE_NUMBER},
      {"Infinity", TYPE_NUMBER},
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    declareVariable(checker, builtins[i].name, (int)strlen(builtins[i].name),
                    builtins[i].type);
  }
}

static TypeKind checkNode(Checker *checker, AstNode *node);

/* Requires a number, reporting against the operator that wanted one. */
static TypeKind requireNumber(Checker *checker, TypeKind type, int line,
                              const char *operatorName) {
  if (csTypeAssignable(type, TYPE_NUMBER)) return TYPE_NUMBER;
  typeError(checker, line, "operand of '%s' must be a number, got %s", operatorName,
            csTypeName(type));
  return TYPE_ERROR;
}

static TypeKind checkBinary(Checker *checker, AstNode *node) {
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
      if (left == TYPE_ANY || right == TYPE_ANY) return TYPE_ANY;
      if (left == TYPE_NUMBER && right == TYPE_NUMBER) return TYPE_NUMBER;
      typeError(checker, line, "cannot add %s and %s", csTypeName(left),
                csTypeName(right));
      return TYPE_ERROR;

    case BINARY_SUBTRACT:
    case BINARY_MULTIPLY:
    case BINARY_DIVIDE:
    case BINARY_MODULO: {
      TypeKind a = requireNumber(checker, left, line, name);
      TypeKind b = requireNumber(checker, right, line, name);
      return (a == TYPE_ERROR || b == TYPE_ERROR) ? TYPE_ERROR : TYPE_NUMBER;
    }

    case BINARY_GREATER:
    case BINARY_GREATER_EQUAL:
    case BINARY_LESS:
    case BINARY_LESS_EQUAL:
      requireNumber(checker, left, line, name);
      requireNumber(checker, right, line, name);
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
        typeError(checker, line,
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

static TypeKind checkNode(Checker *checker, AstNode *node) {
  if (node == NULL) return TYPE_ANY;

  TypeKind result = TYPE_ANY;

  switch (node->type) {
    case AST_NUMBER_LITERAL:    result = TYPE_NUMBER; break;
    case AST_STRING_LITERAL:    result = TYPE_STRING; break;
    case AST_BOOL_LITERAL:      result = TYPE_BOOLEAN; break;
    case AST_NULL_LITERAL:      result = TYPE_NULL; break;
    case AST_UNDEFINED_LITERAL: result = TYPE_UNDEFINED; break;

    case AST_IDENTIFIER: {
      Variable *variable = findVariable(checker, node->as.identifier.name,
                                        node->as.identifier.length);
      /* An unknown name is a runtime error the VM reports with better context,
       * so the checker stays quiet and treats it as dynamic. */
      result = variable != NULL ? variable->type : TYPE_ANY;
      break;
    }

    case AST_ASSIGN: {
      AstNode *target = node->as.assign.target;
      TypeKind valueType = checkNode(checker, node->as.assign.value);
      Variable *variable =
          findVariable(checker, target->as.identifier.name, target->as.identifier.length);

      if (variable != NULL) {
        target->resolvedType = variable->type;
        if (!csTypeAssignable(valueType, variable->type)) {
          typeError(checker, node->line, "cannot assign %s to '%.*s', which is %s",
                    csTypeName(valueType), target->as.identifier.length,
                    target->as.identifier.name, csTypeName(variable->type));
          result = TYPE_ERROR;
          break;
        }
        result = variable->type;
      } else {
        result = valueType;
      }
      break;
    }

    case AST_UPDATE: {
      TypeKind targetType = checkNode(checker, node->as.update.target);
      result = requireNumber(checker, targetType, node->line,
                             node->as.update.isIncrement ? "++" : "--");
      break;
    }

    case AST_UNARY: {
      TypeKind operand = checkNode(checker, node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE:
          result = requireNumber(checker, operand, node->line, "-");
          break;
        case UNARY_NOT:
          result = TYPE_BOOLEAN; /* every type has a truthiness */
          break;
        case UNARY_TYPEOF:
          result = TYPE_STRING;
          break;
      }
      break;
    }

    case AST_BINARY:
      result = checkBinary(checker, node);
      break;

    case AST_LOGICAL: {
      /* `a && b` evaluates to one operand or the other, so the result is only
       * known when both agree. */
      TypeKind left = checkNode(checker, node->as.logical.left);
      TypeKind right = checkNode(checker, node->as.logical.right);
      result = (left == right) ? left : TYPE_ANY;
      break;
    }

    case AST_GROUPING:
      result = checkNode(checker, node->as.grouping);
      break;

    case AST_PROPERTY: {
      TypeKind object = checkNode(checker, node->as.property.object);
      if (csTypeIsKnown(object) && object != TYPE_OBJECT) {
        typeError(checker, node->line, "cannot read property '%.*s' of %s",
                  node->as.property.length, node->as.property.name, csTypeName(object));
        result = TYPE_ERROR;
        break;
      }
      /* Object shapes are not modelled yet, so a property is dynamic. */
      result = TYPE_ANY;
      break;
    }

    case AST_CALL: {
      TypeKind callee = checkNode(checker, node->as.call.callee);
      for (int i = 0; i < node->as.call.argCount; i++) {
        checkNode(checker, node->as.call.arguments[i]);
      }
      if (csTypeIsKnown(callee) && callee != TYPE_FUNCTION) {
        typeError(checker, node->line, "%s is not a function", csTypeName(callee));
        result = TYPE_ERROR;
        break;
      }
      result = TYPE_ANY; /* signatures arrive with user functions */
      break;
    }

    case AST_VAR_DECL: {
      TypeKind initializer = node->as.varDecl.initializer != NULL
                                 ? checkNode(checker, node->as.varDecl.initializer)
                                 : TYPE_UNDEFINED;

      TypeKind declared;
      if (node->as.varDecl.hasAnnotation) {
        declared = node->as.varDecl.declaredType;
        if (!csTypeAssignable(initializer, declared)) {
          typeError(checker, node->line, "cannot assign %s to '%.*s', declared as %s",
                    csTypeName(initializer), node->as.varDecl.length,
                    node->as.varDecl.name, csTypeName(declared));
        }
      } else if (node->as.varDecl.initializer != NULL) {
        /* Inference: an unannotated declaration takes its initialiser's type,
         * which is what lets unannotated code still be checked. */
        declared = initializer;
      } else {
        declared = TYPE_ANY;
      }

      declareVariable(checker, node->as.varDecl.name, node->as.varDecl.length, declared);
      result = declared;
      break;
    }

    case AST_EXPRESSION_STMT:
      checkNode(checker, node->as.expression);
      result = TYPE_UNDEFINED;
      break;

    case AST_BLOCK:
      beginScope(checker);
      for (int i = 0; i < node->as.block.count; i++) {
        checkNode(checker, node->as.block.statements[i]);
      }
      endScope(checker);
      result = TYPE_UNDEFINED;
      break;

    case AST_IF_STMT:
      checkNode(checker, node->as.ifStmt.condition);
      checkNode(checker, node->as.ifStmt.thenBranch);
      checkNode(checker, node->as.ifStmt.elseBranch);
      result = TYPE_UNDEFINED;
      break;

    case AST_WHILE_STMT:
      checkNode(checker, node->as.whileStmt.condition);
      checkNode(checker, node->as.whileStmt.body);
      result = TYPE_UNDEFINED;
      break;

    case AST_FOR_STMT:
      /* The initialiser is scoped to the loop, matching the compiler. */
      beginScope(checker);
      checkNode(checker, node->as.forStmt.initializer);
      checkNode(checker, node->as.forStmt.condition);
      checkNode(checker, node->as.forStmt.increment);
      checkNode(checker, node->as.forStmt.body);
      endScope(checker);
      result = TYPE_UNDEFINED;
      break;

    case AST_PROGRAM:
      for (int i = 0; i < node->as.program.count; i++) {
        checkNode(checker, node->as.program.statements[i]);
      }
      result = TYPE_UNDEFINED;
      break;
  }

  node->resolvedType = result;
  return result;
}

bool csTypeCheck(AstNode *program, Diagnostics *diag) {
  Checker checker;
  checker.diag = diag;
  checker.count = 0;
  checker.scopeDepth = 0;

  declareBuiltins(&checker);
  checkNode(&checker, program);

  return !csDiagnosticsFailed(diag);
}
