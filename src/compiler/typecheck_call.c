/* typecheck_call.c — what a call is, and what it answers.
 *
 * Four ways of knowing, in the order they are asked:
 *
 *   `new C()` answers the shape C declared, when C is a class.
 *   A method named by an interface answers what that method says.
 *   A directly-named function has a declaration the checker can see, with its
 *   defaults, its arity, and — when it is generic — what its type variables
 *   stand for *here*, inferred from the arguments and substituted through.
 *   Anything else falls back on the callee's own type: a parameter annotated
 *   `(n: number) => string` says as much as a declaration does.
 *
 * Only the third can name the function in a message, which is why it is worth
 * keeping apart from the fourth.
 */
#include <string.h>

#include "compiler/typecheck_internal.h"

/* `new Box(3)` — what the class's type variables stand for here.
 *
 * The constructor's parameters are written in terms of them, so matching the
 * arguments against those parameters is the whole of the inference, and it is
 * the same csTypeInfer a generic function call uses.
 *
 * A class with no constructor, or one whose parameters say nothing about a
 * variable, leaves that variable as what the checker could not work out —
 * which is what `new Map()` has always answered. `const m: Map<string, number>
 * = new Map()` is how a program says more, and the annotation carries it. */
static TypeId csTypeInstantiateFromArguments(Checker *checker, AstNode *node, TypeId instance, const TypeId *argTypes) {
  TypeId construct = csTypeConstructorOf(checker->types, instance);
  const CompositeType *shape = csTypeComposite(checker->types, instance);
  if (construct == TYPE_ERROR || shape == NULL || shape->typeParamCount == 0) {
    return csTypeInstantiate(checker->types, instance, NULL, 0);
  }

  TypeId params[CS_MAX_TYPE_PARAMS];
  TypeId bindings[CS_MAX_TYPE_PARAMS];
  int count = shape->typeParamCount;
  for (int i = 0; i < count; i++) {
    params[i] = shape->typeParams[i];
    bindings[i] = TYPE_DYNAMIC;
  }

  const CompositeType *signature = csTypeComposite(checker->types, construct);
  if (signature != NULL && signature->kind == COMPOSITE_FUNCTION) {
    for (int i = 0; i < node->as.call.argCount && i < signature->slotCount; i++) {
      csTypeInfer(checker->types, checker->types->slots[signature->slotStart + i], argTypes[i], params, bindings, count);
    }
  }
  return csTypeInstantiate(checker->types, instance, bindings, count);
}

/* Answers the call's type, having reported anything wrong with it. */
TypeId csTypeCheckCall(Checker *checker, AstNode *node) {
  TypeId result = TYPE_DYNAMIC;
  switch (node->type) {
    case AST_CALL: {
      /* `o.m?.()` is written precisely because `m` might not be there, so the
       * callee is not checked as a property read at all — only the thing it
       * is read from. Otherwise `"ab".nope?.()`, whose whole point is that
       * `nope` is absent, would be a type error. */
      TypeId callee;
      if (node->as.call.optional && node->as.call.callee->type == AST_PROPERTY) {
        checkNode(checker, node->as.call.callee->as.property.object);
        callee = TYPE_DYNAMIC;
      } else {
        callee = checkNode(checker, node->as.call.callee);
      }

      TypeId argTypes[UINT8_MAX];
      for (int i = 0; i < node->as.call.argCount; i++) {
        TypeId argType = checkNode(checker, node->as.call.arguments[i]);
        if (i < UINT8_MAX) argTypes[i] = argType;
      }

      if (node->as.call.optional && (callee == TYPE_NULL || callee == TYPE_UNDEFINED)) {
        result = TYPE_DYNAMIC;
        break;
      }

      /* Calling a `value` is calling something that might be a number. */
      if (csTypeRefuseUnnarrowed(checker, callee, node->line, "calling", node->as.call.callee)) {
        result = TYPE_ERROR;
        break;
      }

      if (csTypeIsKnown(callee) && !csTypeIsCallable(checker->types, callee)) {
        csTypeError(checker, node->line, "%s is not a function", csTypeNameIn(checker->types, callee));
        result = TYPE_ERROR;
        break;
      }

      /* A method declared by an interface says what it answers, which is the
       * whole reason to write one down. */
      if (node->as.call.callee->type == AST_PROPERTY) {
        AstNode *property = node->as.call.callee;
        TypeId receiver = property->as.property.object->resolvedType;
        const TypeMember *member = csTypeFindMember(checker->types, receiver, property->as.property.name, property->as.property.length);
        /* A method is a function-typed member, so the call is checked by the
         * same rule as a call through a variable rather than by a second one
         * for interfaces — arguments included. */
        if (member != NULL && csTypeIs(checker->types, member->type, COMPOSITE_FUNCTION)) {
          result = csTypeCheckCallThrough(checker, node, member->type);
          break;
        }
      }

      /* A built-in method's result is known even though its receiver's element
       * types are not. */
      if (node->as.call.callee->type == AST_PROPERTY && !node->as.call.optional) {
        AstNode *property = node->as.call.callee;
        const MethodSignature *builtin = csTypeFindMethod(property->as.property.object->resolvedType, property->as.property.name, property->as.property.length);
        result = builtin != NULL ? builtin->returns : TYPE_DYNAMIC;
        break;
      }

      /* Only a directly-named callee has a signature the checker can see; a
       * function reached through a property or a parameter stays dynamic. */
      const Signature *signature = NULL;
      if (node->as.call.callee->type == AST_IDENTIFIER) {
        Variable *variable = csTypeFindVariable(checker, node->as.call.callee->as.identifier.name, node->as.call.callee->as.identifier.length);
        if (variable != NULL) signature = variable->signature;
      }

      /* `new Dog()` answers a Dog: a class's name is a type, and the shape
       * behind it was registered where the class was read. Anything else
       * constructed — a native, a class expression, a value holding one — has
       * no shape to answer with. */
      if (node->as.call.isNew) {
        result = TYPE_DYNAMIC;
        /* `new Box<number>(…)` — written down at the site, so there is nothing
         * to work out. The parser resolved it, because that is where a name in
         * a type position is resolved. */
        if (node->as.call.newType != TYPE_DYNAMIC) {
          result = node->as.call.newType;
          break;
        }
        if (node->as.call.callee->type == AST_IDENTIFIER) {
          TypeId instance;
          if (csTypeLookupName(checker->types, node->as.call.callee->as.identifier.name, node->as.call.callee->as.identifier.length, &instance)) {
            if (csTypeTypeParamCount(checker->types, instance) > 0) {
              result = csTypeInstantiateFromArguments(checker, node, instance, argTypes);
            } else if (csTypeIsOpen(checker->types, instance)) {
              result = instance;
            }
          }
        }
        break;
      }

      if (signature == NULL) {
        /* No declaration in sight, but the *type* may still say what this
         * takes and answers — a parameter annotated `(n: number) => string`,
         * an interface's method, a variable holding one. That is what makes a
         * callback checkable at all: before function types existed, everything
         * passed as `Function` was checked only by the runtime. */
        if (csTypeIs(checker->types, callee, COMPOSITE_FUNCTION)) {
          result = csTypeCheckCallThrough(checker, node, callee);
          break;
        }
        /* A union of callables answers one of their results, and what it takes
         * is only agreed where they agree — so the arguments go to the runtime
         * and what comes back is still known. */
        result = csTypeResultOf(checker->types, callee);
        break;
      }

      /* A spread hides how many arguments there really are, so the arity check
       * has to be left to the runtime. */
      bool hasSpread = false;
      for (int i = 0; i < node->as.call.argCount; i++) {
        if (node->as.call.arguments[i]->type == AST_SPREAD) hasSpread = true;
      }

      bool tooMany = !signature->hasRest && node->as.call.argCount > signature->paramCount;
      if (!hasSpread && (node->as.call.argCount < signature->requiredCount || tooMany)) {
        if (signature->requiredCount == signature->paramCount) {
          csTypeError(checker, node->line, "expected %d argument%s but got %d", signature->paramCount, signature->paramCount == 1 ? "" : "s", node->as.call.argCount);
        } else {
          csTypeError(checker, node->line, "expected between %d and %d arguments but got %d", signature->requiredCount, signature->paramCount, node->as.call.argCount);
        }
        result = TYPE_ERROR;
        break;
      }

      /* A generic call is checked against what its type variables stand for
       * *here*: every argument is matched against the parameter it is passed
       * to, and what that says about T is substituted through the rest of the
       * signature before anything is checked. Nothing is written down at the
       * call site — `first([1, 2])` says T is a number by being that call. */
      TypeId paramTypes[UINT8_MAX];
      TypeId returnType = signature->returnType;
      for (int i = 0; i < signature->paramCount && i < UINT8_MAX; i++) paramTypes[i] = signature->paramTypes[i];

      if (signature->typeParamCount > 0) {
        TypeId bindings[CS_MAX_TYPE_PARAMS];
        for (int i = 0; i < signature->typeParamCount; i++) bindings[i] = TYPE_DYNAMIC;

        for (int i = 0; i < node->as.call.argCount && i < signature->paramCount; i++) {
          csTypeInfer(checker->types, signature->paramTypes[i], argTypes[i], signature->typeParams, bindings, signature->typeParamCount);
        }
        for (int i = 0; i < signature->paramCount && i < UINT8_MAX; i++) {
          paramTypes[i] = csTypeSubstitute(checker->types, paramTypes[i], signature->typeParams, bindings, signature->typeParamCount);
        }
        returnType = csTypeSubstitute(checker->types, returnType, signature->typeParams, bindings, signature->typeParamCount);
      }

      for (int i = 0; i < node->as.call.argCount && i < UINT8_MAX; i++) {
        /* Everything past the last declared parameter is collected by the rest
         * parameter, whose annotation describes one argument — so they are all
         * checked against it. Without a rest parameter there is nothing left to
         * check against, and the arity check above has already spoken. */
        int at = i;
        if (at >= signature->paramCount) {
          if (!signature->hasRest) break;
          at = signature->paramCount - 1;
        }
        /* An argument written as a literal is proved against the parameter's
         * interface, the same way a declaration's initialiser is. */
        argTypes[i] = csTypeCheckShape(checker, node->as.call.arguments[i], paramTypes[at]);
        if (csTypeAssignableIn(checker->types, argTypes[i], paramTypes[at])) continue;
        /* `f(undefined)` asks for the default, which is what the language
         * already says an absent argument means. */
        if (argTypes[i] == TYPE_UNDEFINED && signature->paramHasDefault[at]) continue;
        csTypeError(checker, node->line, "argument %d of '%.*s' is %s but the parameter is %s", i + 1, node->as.call.callee->as.identifier.length,
                    node->as.call.callee->as.identifier.name, csTypeNameIn(checker->types, argTypes[i]), csTypeNameIn(checker->types, paramTypes[at]));
      }

      result = returnType;
      break;
    }

    default: break;
  }
  return result;
}
