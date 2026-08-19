#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#define MAX_SCOPED_VARIABLES 512

/* A variable the checker knows about. The scope stack mirrors the compiler's,
 * deliberately: keeping the two passes independent means the compiler can be
 * changed without silently altering what is or is not an error. */
/* A function's declared shape. Parameter and return types are kept here rather
 * than in TypeKind, which stays a flat enum: a full type tree is only worth
 * building once object shapes and generics need one. */
typedef struct {
  TypeKind returnType;
  bool hasReturnAnnotation;
  int paramCount;
  TypeKind paramTypes[UINT8_MAX];
} Signature;

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
    declareVariable(checker, builtins[i].name, (int)strlen(builtins[i].name),
                    builtins[i].type);
  }
}

/* What the built-in methods return, so `"a".toUpperCase()` is a string rather
 * than dynamic. Only the return type is modelled; argument checking is left to
 * the runtime, because several of these are variadic or accept several shapes.
 *
 * TYPE_ANY here means "known method, unknown result" — an array method whose
 * element type the checker cannot see. */
typedef struct {
  TypeKind receiver;
  const char *name;
  TypeKind returns;
} MethodSignature;

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
    {TYPE_STRING, "split", TYPE_OBJECT},
    {TYPE_STRING, "padStart", TYPE_STRING},
    {TYPE_STRING, "padEnd", TYPE_STRING},
    {TYPE_STRING, "concat", TYPE_STRING},

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
};

/* Returns the signature for `name` on `receiver`, or NULL. */
static const MethodSignature *findMethod(TypeKind receiver, const char *name,
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

static TypeKind checkNode(Checker *checker, AstNode *node);

/* Records a function's shape and binds its name, before the body is walked so
 * that recursive calls resolve. */
static const Signature *declareFunction(Checker *checker, AstNode *node) {
  if (checker->signatureCount >= MAX_FUNCTIONS) return NULL;

  Signature *signature = &checker->signatures[checker->signatureCount++];
  signature->returnType =
      node->as.function.hasReturnAnnotation ? node->as.function.returnType : TYPE_ANY;
  signature->hasReturnAnnotation = node->as.function.hasReturnAnnotation;
  signature->paramCount = node->as.function.paramCount;
  for (int i = 0; i < node->as.function.paramCount && i < UINT8_MAX; i++) {
    const AstParam *param = &node->as.function.params[i];
    signature->paramTypes[i] = param->hasAnnotation ? param->type : TYPE_ANY;
  }

  if (node->as.function.name != NULL && !node->as.function.nameIsInferred) {
    declareVariable(checker, node->as.function.name, node->as.function.nameLength,
                    TYPE_FUNCTION);
    checker->variables[checker->count - 1].signature = signature;
  }
  return signature;
}

static void checkFunctionBody(Checker *checker, AstNode *node,
                              const Signature *signature) {
  TypeKind savedReturn = checker->currentReturn;
  bool savedAnnotated = checker->currentReturnAnnotated;
  checker->currentReturn = signature != NULL ? signature->returnType : TYPE_ANY;
  checker->currentReturnAnnotated =
      signature != NULL && signature->hasReturnAnnotation;
  checker->functionDepth++;

  beginScope(checker);
  for (int i = 0; i < node->as.function.paramCount; i++) {
    const AstParam *param = &node->as.function.params[i];
    declareVariable(checker, param->name, param->length,
                    param->hasAnnotation ? param->type : TYPE_ANY);
  }
  /* The body is an AST_BLOCK, but its statements are checked in the scope that
   * already holds the parameters rather than in one nested inside it. */
  for (int i = 0; i < node->as.function.body->as.block.count; i++) {
    checkNode(checker, node->as.function.body->as.block.statements[i]);
  }
  endScope(checker);

  checker->functionDepth--;
  checker->currentReturn = savedReturn;
  checker->currentReturnAnnotated = savedAnnotated;
}

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

    case BINARY_INSTANCEOF:
      /* The right side has to be a class, which the checker cannot see, so the
       * VM does that test. The answer is a boolean either way. */
      checkNode(checker, node->as.binary.left);
      return TYPE_BOOLEAN;

    case BINARY_SUBTRACT:
    case BINARY_MULTIPLY:
    case BINARY_DIVIDE:
    case BINARY_MODULO:
    case BINARY_EXPONENT: {
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

      /* Property and index targets have no declared type to check against. */
      if (target->type != AST_IDENTIFIER) {
        checkNode(checker, target);
        result = valueType;
        break;
      }

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
      bool isLength = node->as.property.length == 6 &&
                      memcmp(node->as.property.name, "length", 6) == 0;

      /* `length` is a number on every container; a known method name resolves
       * to a function, and its result type is applied at the call site. */
      if (object == TYPE_STRING) {
        if (isLength) {
          result = TYPE_NUMBER;
          break;
        }
        if (findMethod(TYPE_STRING, node->as.property.name,
                       node->as.property.length) != NULL) {
          result = TYPE_FUNCTION;
          break;
        }
        typeError(checker, node->line, "strings have no property '%.*s'",
                  node->as.property.length, node->as.property.name);
        result = TYPE_ERROR;
        break;
      }

      /* A callable may carry statics — `Number.isInteger` sits on the same
       * value `Number(x)` calls — so a property read on a function is allowed
       * and simply dynamic. */
      if (csTypeIsKnown(object) && object != TYPE_OBJECT && object != TYPE_FUNCTION) {
        typeError(checker, node->line, "cannot read property '%.*s' of %s",
                  node->as.property.length, node->as.property.name, csTypeName(object));
        result = TYPE_ERROR;
        break;
      }

      /* Object shapes are not modelled yet, so a property is dynamic. That
       * includes `length`: arrays and plain objects share TYPE_OBJECT here, so
       * assuming a number would reject `class Queue { length() { ... } }` —
       * and being wrong about a type is worse than not knowing it. A string's
       * `length` is handled above, where it really is guaranteed. */
      (void)isLength;
      result = TYPE_ANY;
      break;
    }

    case AST_CALL: {
      TypeKind callee = checkNode(checker, node->as.call.callee);

      TypeKind argTypes[UINT8_MAX];
      for (int i = 0; i < node->as.call.argCount; i++) {
        TypeKind argType = checkNode(checker, node->as.call.arguments[i]);
        if (i < UINT8_MAX) argTypes[i] = argType;
      }

      if (csTypeIsKnown(callee) && callee != TYPE_FUNCTION) {
        typeError(checker, node->line, "%s is not a function", csTypeName(callee));
        result = TYPE_ERROR;
        break;
      }

      /* A built-in method's result is known even though its receiver's element
       * types are not. */
      if (node->as.call.callee->type == AST_PROPERTY) {
        AstNode *property = node->as.call.callee;
        const MethodSignature *builtin =
            findMethod(property->as.property.object->resolvedType,
                       property->as.property.name, property->as.property.length);
        result = builtin != NULL ? builtin->returns : TYPE_ANY;
        break;
      }

      /* Only a directly-named callee has a signature the checker can see; a
       * function reached through a property or a parameter stays dynamic. */
      const Signature *signature = NULL;
      if (node->as.call.callee->type == AST_IDENTIFIER) {
        Variable *variable = findVariable(checker, node->as.call.callee->as.identifier.name,
                                          node->as.call.callee->as.identifier.length);
        if (variable != NULL) signature = variable->signature;
      }

      if (signature == NULL) {
        result = TYPE_ANY;
        break;
      }

      /* A spread hides how many arguments there really are, so the arity check
       * has to be left to the runtime. */
      bool hasSpread = false;
      for (int i = 0; i < node->as.call.argCount; i++) {
        if (node->as.call.arguments[i]->type == AST_SPREAD) hasSpread = true;
      }

      if (!hasSpread && node->as.call.argCount != signature->paramCount) {
        typeError(checker, node->line, "expected %d argument%s but got %d",
                  signature->paramCount, signature->paramCount == 1 ? "" : "s",
                  node->as.call.argCount);
        result = TYPE_ERROR;
        break;
      }

      for (int i = 0; i < node->as.call.argCount && i < UINT8_MAX; i++) {
        if (!csTypeAssignable(argTypes[i], signature->paramTypes[i])) {
          typeError(checker, node->line,
                    "argument %d is %s but the parameter is %s", i + 1,
                    csTypeName(argTypes[i]), csTypeName(signature->paramTypes[i]));
        }
      }

      result = signature->returnType;
      break;
    }

    case AST_INDEX: {
      TypeKind target = checkNode(checker, node->as.index.target);
      checkNode(checker, node->as.index.index);
      if (csTypeIsKnown(target) && target != TYPE_OBJECT && target != TYPE_STRING) {
        typeError(checker, node->line, "cannot index %s", csTypeName(target));
        result = TYPE_ERROR;
        break;
      }
      /* Element types are not modelled, so an index is dynamic. */
      result = TYPE_ANY;
      break;
    }

    case AST_OBJECT_LITERAL:
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        checkNode(checker, node->as.objectLiteral.values[i]);
      }
      result = TYPE_OBJECT;
      break;

    case AST_ARRAY_LITERAL:
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        checkNode(checker, node->as.arrayLiteral.elements[i]);
      }
      /* Arrays are objects for typing purposes until element types exist. */
      result = TYPE_OBJECT;
      break;

    case AST_CONDITIONAL: {
      checkNode(checker, node->as.conditional.condition);
      TypeKind thenType = checkNode(checker, node->as.conditional.thenValue);
      TypeKind elseType = checkNode(checker, node->as.conditional.elseValue);
      /* Without union types the result is only known when both arms agree. */
      result = thenType == elseType ? thenType : TYPE_ANY;
      break;
    }

    case AST_BREAK_STMT:
    case AST_CONTINUE_STMT:
      /* The compiler reports these when they are out of place, where it knows
       * the enclosing loop. */
      result = TYPE_UNDEFINED;
      break;

    case AST_SWITCH_STMT: {
      TypeKind subject = checkNode(checker, node->as.switchStmt.subject);
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        TypeKind test = checkNode(checker, node->as.switchStmt.cases[i].test);
        /* Arms are matched with ===, so an arm that can never match is the
         * same mistake as writing that comparison out by hand. */
        if (csTypeIsKnown(subject) && csTypeIsKnown(test) && subject != test) {
          typeError(checker, node->as.switchStmt.cases[i].test->line,
                    "this case is %s but the switch subject is %s, so it can never match",
                    csTypeName(test), csTypeName(subject));
        }
        checkNode(checker, node->as.switchStmt.cases[i].body);
      }
      checkNode(checker, node->as.switchStmt.defaultBody);
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_FUNCTION: {
      const Signature *signature = declareFunction(checker, node);
      checkFunctionBody(checker, node, signature);
      result = TYPE_FUNCTION;
      break;
    }

    case AST_RETURN_STMT: {
      TypeKind returned = node->as.returnValue != NULL
                              ? checkNode(checker, node->as.returnValue)
                              : TYPE_UNDEFINED;
      if (checker->functionDepth == 0) {
        /* The compiler reports this with better placement. */
        result = TYPE_UNDEFINED;
        break;
      }
      if (checker->currentReturnAnnotated &&
          !csTypeAssignable(returned, checker->currentReturn)) {
        typeError(checker, node->line, "cannot return %s from a function declared %s",
                  csTypeName(returned), csTypeName(checker->currentReturn));
      }
      result = TYPE_UNDEFINED;
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

    case AST_SPREAD:
      checkNode(checker, node->as.spread);
      result = TYPE_ANY;
      break;

    case AST_DESTRUCTURE:
      checkNode(checker, node->as.destructure.initializer);
      for (int i = 0; i < node->as.destructure.count; i++) {
        checkNode(checker, node->as.destructure.bindings[i].defaultValue);
        /* Element and property types are not modelled, so each binding is
         * dynamic. */
        declareVariable(checker, node->as.destructure.bindings[i].name,
                        node->as.destructure.bindings[i].nameLength, TYPE_ANY);
      }
      result = TYPE_UNDEFINED;
      break;

    case AST_TRY_STMT:
      checkNode(checker, node->as.tryStmt.body);
      if (node->as.tryStmt.catchBody != NULL) {
        beginScope(checker);
        if (node->as.tryStmt.catchName != NULL) {
          /* Anything can be thrown, so the binding is dynamic. */
          declareVariable(checker, node->as.tryStmt.catchName,
                          node->as.tryStmt.catchNameLength, TYPE_ANY);
        }
        checkNode(checker, node->as.tryStmt.catchBody);
        endScope(checker);
      }
      checkNode(checker, node->as.tryStmt.finallyBody);
      result = TYPE_UNDEFINED;
      break;

    case AST_THROW_STMT:
      checkNode(checker, node->as.thrown);
      result = TYPE_UNDEFINED;
      break;

    /* Class types are not modelled. The lattice here is a fixed set of
     * primitives, and adding nominal types with members and subtyping is a
     * milestone of its own — so an instance is `object`, a class is dynamic,
     * and `this` is dynamic. Nothing about a class is checked statically
     * beyond what its method bodies say on their own. */
    case AST_THIS:
    case AST_SUPER:
      result = TYPE_ANY;
      break;

    case AST_CLASS_DECL: {
      declareVariable(checker, node->as.classDecl.name, node->as.classDecl.nameLength,
                      TYPE_ANY);

      for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
        checkNode(checker, node->as.classDecl.fields[i].initializer);
      }
      if (node->as.classDecl.constructor != NULL) {
        checkNode(checker, node->as.classDecl.constructor);
      }
      for (int i = 0; i < node->as.classDecl.memberCount; i++) {
        checkNode(checker, node->as.classDecl.members[i].function);
      }
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_FOR_OF_STMT:
      beginScope(checker);
      checkNode(checker, node->as.forOf.iterable);
      /* Element types are not modelled, so the binding is dynamic. */
      declareVariable(checker, node->as.forOf.name, node->as.forOf.nameLength,
                      TYPE_ANY);
      checkNode(checker, node->as.forOf.body);
      endScope(checker);
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
  checker.signatureCount = 0;
  checker.currentReturn = TYPE_ANY;
  checker.currentReturnAnnotated = false;
  checker.functionDepth = 0;

  declareBuiltins(&checker);
  checkNode(&checker, program);

  return !csDiagnosticsFailed(diag);
}
