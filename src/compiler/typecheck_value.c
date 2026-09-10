/* typecheck_value.c — the type of an expression.
 *
 * Every one of these answers what the expression evaluates to, and reports
 * where the answer cannot be what the program needs. `any` is the escape hatch
 * and is assignable in both directions, which is the one place the checker
 * deliberately stops being sound — that is what makes the system gradual
 * rather than static.
 *
 * one of the two groups checkNode asks. Answers false for a node
 * belonging to the other, so a node neither claims keeps the `any` the checker
 * has always fallen back to. See typecheck.c for the walk and
 * typecheck_internal.h for the state they share.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#include "compiler/typecheck_internal.h"

bool checkValueNode(Checker *checker, AstNode *node, TypeKind *out) {
  TypeKind result = TYPE_ANY;

  switch (node->type) {
    case AST_NUMBER_LITERAL:    result = TYPE_NUMBER; break;
    case AST_STRING_LITERAL:    result = TYPE_STRING; break;
    case AST_BIGINT_LITERAL:    result = TYPE_BIGINT; break;
    case AST_BOOL_LITERAL:      result = TYPE_BOOLEAN; break;
    case AST_NULL_LITERAL:      result = TYPE_NULL; break;
    case AST_UNDEFINED_LITERAL: result = TYPE_UNDEFINED; break;

    case AST_IDENTIFIER: {
      Variable *variable = csTypeFindVariable(checker, node->as.identifier.name,
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
          csTypeFindVariable(checker, target->as.identifier.name, target->as.identifier.length);

      if (variable != NULL) {
        target->resolvedType = variable->type;
        if (!csTypeAssignable(valueType, variable->type)) {
          csTypeError(checker, node->line, "cannot assign %s to '%.*s', which is %s",
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
      result = csTypeRequireNumber(checker, targetType, node->line,
                             node->as.update.isIncrement ? "++" : "--");
      break;
    }

    case AST_UNARY: {
      TypeKind operand = checkNode(checker, node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE:
          /* `-1n` is a BigInt; every other operand has to be a number. */
          result = operand == TYPE_BIGINT
                       ? TYPE_BIGINT
                       : csTypeRequireNumber(checker, operand, node->line, "-");
          break;
        case UNARY_NOT:
          result = TYPE_BOOLEAN; /* every type has a truthiness */
          break;
        case UNARY_TYPEOF:
          result = TYPE_STRING;
          break;
        case UNARY_VOID:
          result = TYPE_UNDEFINED;
          break;
      }
      break;
    }

    case AST_BINARY:
      result = csTypeCheckBinary(checker, node);
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

    /* A chain can short-circuit to undefined whatever its links say, so the
     * type it produces is not the type of the last one. */
    case AST_TEMPLATE_STRINGS:
      checkNode(checker, node->as.templateStrings.cooked);
      checkNode(checker, node->as.templateStrings.raw);
      result = TYPE_OBJECT;
      break;

    case AST_SEQUENCE:
      checkNode(checker, node->as.sequence.first);
      result = checkNode(checker, node->as.sequence.second);
      break;

    case AST_YIELD:
      /* What `next(x)` sends back in is whatever the caller chose. */
      checkNode(checker, node->as.yield.value);
      result = TYPE_ANY;
      break;

    case AST_DELETE:
      /* Whether the property was there is not a question about types. */
      checkNode(checker, node->as.deleteTarget);
      result = TYPE_BOOLEAN;
      break;

    case AST_OPTIONAL_CHAIN:
      checkNode(checker, node->as.expression);
      result = TYPE_ANY;
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
        if (csTypeFindMethod(TYPE_STRING, node->as.property.name,
                       node->as.property.length) != NULL) {
          result = TYPE_FUNCTION;
          break;
        }
        csTypeError(checker, node->line, "strings have no property '%.*s'",
                  node->as.property.length, node->as.property.name);
        result = TYPE_ERROR;
        break;
      }

      /* Any other primitive with methods of its own — a number, so far. The
       * table is asked before the type is rejected, which is what keeps the
       * rule "a primitive has no properties" from being wrong the moment one
       * gains a method. */
      if (csTypeFindMethod(object, node->as.property.name,
                     node->as.property.length) != NULL) {
        result = TYPE_FUNCTION;
        break;
      }

      /* A callable may carry statics — `Number.isInteger` sits on the same
       * value `Number(x)` calls — so a property read on a function is allowed
       * and simply dynamic. */
      /* `?.` says the receiver may be absent, so a null or undefined one is
       * the case being handled rather than a mistake. */
      if (node->as.property.optional &&
          (object == TYPE_NULL || object == TYPE_UNDEFINED)) {
        result = TYPE_ANY;
        break;
      }

      if (csTypeIsKnown(object) && object != TYPE_OBJECT && object != TYPE_FUNCTION) {
        csTypeError(checker, node->line, "cannot read property '%.*s' of %s",
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
      /* `o.m?.()` is written precisely because `m` might not be there, so the
       * callee is not checked as a property read at all — only the thing it
       * is read from. Otherwise `"ab".nope?.()`, whose whole point is that
       * `nope` is absent, would be a type error. */
      TypeKind callee;
      if (node->as.call.optional && node->as.call.callee->type == AST_PROPERTY) {
        checkNode(checker, node->as.call.callee->as.property.object);
        callee = TYPE_ANY;
      } else {
        callee = checkNode(checker, node->as.call.callee);
      }

      TypeKind argTypes[UINT8_MAX];
      for (int i = 0; i < node->as.call.argCount; i++) {
        TypeKind argType = checkNode(checker, node->as.call.arguments[i]);
        if (i < UINT8_MAX) argTypes[i] = argType;
      }

      if (node->as.call.optional &&
          (callee == TYPE_NULL || callee == TYPE_UNDEFINED)) {
        result = TYPE_ANY;
        break;
      }

      if (csTypeIsKnown(callee) && callee != TYPE_FUNCTION) {
        csTypeError(checker, node->line, "%s is not a function", csTypeName(callee));
        result = TYPE_ERROR;
        break;
      }

      /* A built-in method's result is known even though its receiver's element
       * types are not. */
      if (node->as.call.callee->type == AST_PROPERTY && !node->as.call.optional) {
        AstNode *property = node->as.call.callee;
        const MethodSignature *builtin =
            csTypeFindMethod(property->as.property.object->resolvedType,
                       property->as.property.name, property->as.property.length);
        result = builtin != NULL ? builtin->returns : TYPE_ANY;
        break;
      }

      /* Only a directly-named callee has a signature the checker can see; a
       * function reached through a property or a parameter stays dynamic. */
      const Signature *signature = NULL;
      if (node->as.call.callee->type == AST_IDENTIFIER) {
        Variable *variable = csTypeFindVariable(checker, node->as.call.callee->as.identifier.name,
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

      bool tooMany = !signature->hasRest &&
                     node->as.call.argCount > signature->paramCount;
      if (!hasSpread &&
          (node->as.call.argCount < signature->requiredCount || tooMany)) {
        if (signature->requiredCount == signature->paramCount) {
          csTypeError(checker, node->line, "expected %d argument%s but got %d",
                    signature->paramCount, signature->paramCount == 1 ? "" : "s",
                    node->as.call.argCount);
        } else {
          csTypeError(checker, node->line,
                    "expected between %d and %d arguments but got %d",
                    signature->requiredCount, signature->paramCount,
                    node->as.call.argCount);
        }
        result = TYPE_ERROR;
        break;
      }

      for (int i = 0; i < node->as.call.argCount && i < UINT8_MAX; i++) {
        if (!csTypeAssignable(argTypes[i], signature->paramTypes[i])) {
          csTypeError(checker, node->line,
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
      /* `?.[` says the target may be absent; that is the case being handled. */
      if (node->as.index.optional &&
          (target == TYPE_NULL || target == TYPE_UNDEFINED)) {
        result = TYPE_ANY;
        break;
      }
      if (csTypeIsKnown(target) && target != TYPE_OBJECT && target != TYPE_STRING) {
        csTypeError(checker, node->line, "cannot index %s", csTypeName(target));
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
        csTypeDeclareVariable(checker, node->as.destructure.bindings[i].name,
                        node->as.destructure.bindings[i].nameLength, TYPE_ANY);
      }
      result = TYPE_UNDEFINED;
      break;

    case AST_REGEX_LITERAL:
      result = TYPE_OBJECT;
      break;

    case AST_DYNAMIC_IMPORT:
      checkNode(checker, node->as.unary.operand);
      result = TYPE_ANY; /* a promise, which the lattice does not model */
      break;

    case AST_THIS:
    case AST_NEW_TARGET:
    case AST_SUPER:
      result = TYPE_ANY;
      break;

    /* What a promise resolves to is not modelled, so awaiting one is dynamic.
     * An async function's declared return type describes what it resolves to
     * rather than what calling it produces, so it is not checked either. */
    case AST_AWAIT:
      checkNode(checker, node->as.unary.operand);
      result = TYPE_ANY;
      break;

    /* Types do not cross a module boundary yet: the checker runs per file and
     * has no record of what another file resolved. An imported binding is
     * dynamic, which is honest rather than merely permissive. */

    default:
      return false;
  }

  *out = result;
  return true;
}
