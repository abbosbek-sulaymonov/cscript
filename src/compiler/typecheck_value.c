/* typecheck_value.c — the type of an expression.
 *
 * Every one of these answers what the expression evaluates to, and reports
 * where the answer cannot be what the program needs. Two answers are not
 * ordinary types: `value`, which the program may write and must narrow before
 * using, and the dynamic type, which it may not write at all and which stands
 * for what the checker could not see — an array's elements, an object's
 * properties, what an import holds. Only the second is assignable in both
 * directions, and it is the one place the checking stops being static; the
 * runtime takes over there, at the boundary vm_call.inc guards.
 *
 * This file is one of the two groups checkNode asks. It answers false for a
 * node belonging to the other, so a node neither claims keeps the dynamic type
 * the checker falls back to. See typecheck.c for the walk and
 * typecheck_internal.h for the state they share.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#include "compiler/typecheck_internal.h"

bool checkValueNode(Checker *checker, AstNode *node, TypeId *out) {
  TypeId result = TYPE_DYNAMIC;

  switch (node->type) {
    case AST_NUMBER_LITERAL: result = TYPE_NUMBER; break;
    case AST_STRING_LITERAL: result = TYPE_STRING; break;
    case AST_BIGINT_LITERAL: result = TYPE_BIGINT; break;
    case AST_BOOL_LITERAL: result = TYPE_BOOLEAN; break;
    case AST_NULL_LITERAL: result = TYPE_NULL; break;
    case AST_UNDEFINED_LITERAL: result = TYPE_UNDEFINED; break;

    case AST_IDENTIFIER: {
      Variable *variable = csTypeFindVariable(checker, node->as.identifier.name, node->as.identifier.length);
      /* An unknown name is a runtime error the VM reports with better context,
       * so the checker stays quiet and treats it as dynamic. */
      result = variable != NULL ? variable->type : TYPE_DYNAMIC;
      break;
    }

    case AST_ASSIGN: {
      AstNode *target = node->as.assign.target;
      TypeId valueType = checkNode(checker, node->as.assign.value);

      /* Property and index targets have no declared type to check against. */
      if (target->type != AST_IDENTIFIER) {
        checkNode(checker, target);
        result = valueType;
        break;
      }

      Variable *variable = csTypeFindVariable(checker, target->as.identifier.name, target->as.identifier.length);

      if (variable != NULL && variable->awaiting) {
        /* The first assignment is what says what this is. `null` and
         * `undefined` say "still nothing", so they do not settle it — which is
         * what keeps `let x; x = null; x = 5;` working and `x = 5; x = "s"`
         * an error. */
        if (valueType != TYPE_NULL && valueType != TYPE_UNDEFINED && valueType != TYPE_ERROR) {
          variable->type = valueType;
          variable->awaiting = false;
        }
        target->resolvedType = variable->type;
        result = variable->type;
        break;
      }

      if (variable != NULL) {
        target->resolvedType = variable->type;
        /* A literal assigned to something an interface named is proved here,
         * the same way a declaration's initialiser is. */
        valueType = csTypeCheckShape(checker, node->as.assign.value, variable->type);
        if (!csTypeAssignableIn(checker->types, valueType, variable->type)) {
          csTypeError(checker, node->line, "cannot assign %s to '%.*s', which is %s and cannot change type", csTypeNameIn(checker->types, valueType),
                      target->as.identifier.length, target->as.identifier.name, csTypeNameIn(checker->types, variable->type));
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
      TypeId targetType = checkNode(checker, node->as.update.target);
      result = csTypeRequireNumber(checker, targetType, node->line, node->as.update.isIncrement ? "++" : "--", node->as.update.target);
      break;
    }

    case AST_UNARY: {
      TypeId operand = checkNode(checker, node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE:
          /* `-1n` is a BigInt; every other operand has to be a number. */
          result = operand == TYPE_BIGINT ? TYPE_BIGINT : csTypeRequireNumber(checker, operand, node->line, "-", node->as.unary.operand);
          break;
        case UNARY_NOT:
          result = TYPE_BOOLEAN; /* every type has a truthiness */
          break;
        case UNARY_TYPEOF: result = TYPE_STRING; break;
        case UNARY_VOID: result = TYPE_UNDEFINED; break;
      }
      break;
    }

    case AST_BINARY: result = csTypeCheckBinary(checker, node); break;

    case AST_LOGICAL: {
      /* `a && b` evaluates to one operand or the other, so the result is only
       * known when both agree. */
      TypeId left = checkNode(checker, node->as.logical.left);

      /* The right-hand side only runs when the left said what it said, so it
       * is checked knowing that: `typeof x === "number" && x > 0` is the usual
       * way a guard is written, and `typeof x !== "number" || x < 0` is the
       * same guard inverted. */
      TypeId saved = TYPE_DYNAMIC;
      Variable *narrowed = csTypeNarrow(checker, node->as.logical.left, node->as.logical.op == LOGICAL_AND, &saved);
      TypeId right = checkNode(checker, node->as.logical.right);
      if (narrowed != NULL) narrowed->type = saved;

      result = (left == right) ? left : TYPE_DYNAMIC;
      break;
    }

    case AST_GROUPING: result = checkNode(checker, node->as.grouping); break;

    /* A chain can short-circuit to undefined whatever its links say, so the
     * type it produces is not the type of the last one. */
    case AST_TEMPLATE_STRINGS:
      /* The pieces a tagged template hands its tag: an array of strings, with
       * `raw` hanging off it. A tag declared `(parts: string[], ...)` is
       * therefore checked against what it will really be given. */
      checkNode(checker, node->as.templateStrings.cooked);
      checkNode(checker, node->as.templateStrings.raw);
      result = csTypeArrayOf(checker->types, TYPE_STRING);
      break;

    case AST_SEQUENCE:
      checkNode(checker, node->as.sequence.first);
      result = checkNode(checker, node->as.sequence.second);
      break;

    case AST_YIELD:
      /* What `next(x)` sends back in is whatever the caller chose. */
      checkNode(checker, node->as.yield.value);
      result = TYPE_DYNAMIC;
      break;

    case AST_DELETE:
      /* Whether the property was there is not a question about types. */
      checkNode(checker, node->as.deleteTarget);
      result = TYPE_BOOLEAN;
      break;

    case AST_OPTIONAL_CHAIN:
      checkNode(checker, node->as.expression);
      result = TYPE_DYNAMIC;
      break;

    case AST_PROPERTY: {
      TypeId object = checkNode(checker, node->as.property.object);

      /* Reading a property off a `value` is the commonest way a program would
       * use one without knowing what it is. */
      if (csTypeRefuseUnnarrowed(checker, object, node->line, "reading a property", node->as.property.object)) {
        result = TYPE_ERROR;
        break;
      }
      bool isLength = node->as.property.length == 6 && memcmp(node->as.property.name, "length", 6) == 0;

      /* An interface is the one object type whose members are known, so this
       * is the one property read that answers something better than "an
       * object, contents unknown" — and the one that can say a name is wrong
       * before the program runs. */
      if (csTypeIs(checker->types, object, COMPOSITE_INTERFACE)) {
        const TypeMember *member = csTypeFindMember(checker->types, object, node->as.property.name, node->as.property.length);
        /* A class may add a field its declaration never mentioned — `this.name
         * = name` is the commonest line in a constructor — so a member it did
         * not declare is dynamic rather than an error. An interface declares
         * everything it has, and says so. */
        if (member == NULL && csTypeIsOpen(checker->types, object)) {
          result = TYPE_DYNAMIC;
          break;
        }
        if (member == NULL) {
          csTypeError(checker, node->line, "%s has no member '%.*s'", csTypeNameIn(checker->types, object), node->as.property.length, node->as.property.name);
          result = TYPE_ERROR;
          break;
        }
        result = member->type;
        break;
      }

      /* `length` is a number on every container; a known method name resolves
       * to a function, and its result type is applied at the call site. */
      if (object == TYPE_STRING) {
        if (isLength) {
          result = TYPE_NUMBER;
          break;
        }
        if (csTypeFindMethod(TYPE_STRING, node->as.property.name, node->as.property.length) != NULL) {
          result = TYPE_FUNCTION;
          break;
        }
        csTypeError(checker, node->line, "strings have no property '%.*s'", node->as.property.length, node->as.property.name);
        result = TYPE_ERROR;
        break;
      }

      /* Any other primitive with methods of its own — a number, so far. The
       * table is asked before the type is rejected, which is what keeps the
       * rule "a primitive has no properties" from being wrong the moment one
       * gains a method.
       *
       * TYPE_OBJECT is excluded on purpose. An array and a plain object are
       * the same type here, so the array methods are in the table under
       * TYPE_OBJECT — and answering "function" for any property whose *name*
       * matches one of them means an ordinary field cannot be called `at`,
       * `sort`, `filter`, `map` or `includes`. `{ sort: "name" }.sort` is a
       * string, and typing it as a function made `o.sort < 5` a compile error
       * on correct code. A property of an object stays dynamic; a call through
       * one still gets its result type from this same table, which is what
       * keeps `xs.map(f)` known to be an array. */
      /* An array's `length`, which is the one property it really has. */
      if (csTypeIsArrayLike(checker->types, object) && isLength) {
        result = TYPE_NUMBER;
        break;
      }

      if (csTypeIsArrayLike(checker->types, object)) {
        /* A method of an array, whose result the table below knows. Asked
         * against the bare kind, because `number[]` and `array` answer the
         * same method names. */
        if (csTypeFindMethod(TYPE_ARRAY, node->as.property.name, node->as.property.length) != NULL) {
          result = TYPE_FUNCTION;
          break;
        }
        result = TYPE_DYNAMIC;
        break;
      }

      if (object != TYPE_OBJECT && csTypeFindMethod(object, node->as.property.name, node->as.property.length) != NULL) {
        result = TYPE_FUNCTION;
        break;
      }

      /* A callable may carry statics — `Number.isInteger` sits on the same
       * value `Number(x)` calls — so a property read on a function is allowed
       * and simply dynamic. */
      /* `?.` says the receiver may be absent, so a null or undefined one is
       * the case being handled rather than a mistake. */
      if (node->as.property.optional && (object == TYPE_NULL || object == TYPE_UNDEFINED)) {
        result = TYPE_DYNAMIC;
        break;
      }

      if (csTypeIsKnown(object) && object != TYPE_OBJECT && !csTypeIsArrayLike(checker->types, object) && !csTypeIsCallable(checker->types, object)) {
        csTypeError(checker, node->line, "cannot read property '%.*s' of %s", node->as.property.length, node->as.property.name, csTypeNameIn(checker->types, object));
        result = TYPE_ERROR;
        break;
      }

      /* Object shapes are not modelled yet, so a property is dynamic. That
       * includes `length`: arrays and plain objects share TYPE_OBJECT here, so
       * assuming a number would reject `class Queue { length() { ... } }` —
       * and being wrong about a type is worse than not knowing it. A string's
       * `length` is handled above, where it really is guaranteed. */
      (void)isLength;
      result = TYPE_DYNAMIC;
      break;
    }

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
        if (node->as.call.callee->type == AST_IDENTIFIER) {
          TypeId instance;
          if (csTypeLookupName(checker->types, node->as.call.callee->as.identifier.name, node->as.call.callee->as.identifier.length, &instance) &&
              csTypeIsOpen(checker->types, instance)) {
            result = instance;
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
        result = TYPE_DYNAMIC;
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

    case AST_INDEX: {
      TypeId target = checkNode(checker, node->as.index.target);
      checkNode(checker, node->as.index.index);
      /* `?.[` says the target may be absent; that is the case being handled. */
      if (node->as.index.optional && (target == TYPE_NULL || target == TYPE_UNDEFINED)) {
        result = TYPE_DYNAMIC;
        break;
      }
      if (csTypeRefuseUnnarrowed(checker, target, node->line, "indexing", node->as.index.target)) {
        result = TYPE_ERROR;
        break;
      }
      /* A shape is an object, so it may be indexed — with a key the checker
       * cannot read, which is why the answer below is dynamic rather than a
       * member's type. */
      bool indexableShape = csTypeIs(checker->types, target, COMPOSITE_INTERFACE);
      if (csTypeIsKnown(target) && target != TYPE_OBJECT && !indexableShape && !csTypeIsArrayLike(checker->types, target) && target != TYPE_STRING) {
        csTypeError(checker, node->line, "cannot index %s", csTypeNameIn(checker->types, target));
        result = TYPE_ERROR;
        break;
      }
      /* `xs[0]` on a `number[]` is a number. On a bare `array` it is what the
       * checker could not work out, which is what a bare `array` means.
       *
       * A *string* key is not an element at all: `xs["sort"]` is the method
       * of that name, reached the long way round. Telling the two apart is
       * what the index's own type is for. */
      if (csTypeIsArrayLike(checker->types, target) && node->as.index.index->resolvedType == TYPE_STRING) {
        result = TYPE_DYNAMIC;
        break;
      }
      result = csTypeElementOf(checker->types, target);
      break;
    }

    case AST_OBJECT_LITERAL:
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        checkNode(checker, node->as.objectLiteral.values[i]);
      }
      result = TYPE_OBJECT;
      break;

    case AST_ARRAY_LITERAL: {
      /* An array, not an object: the two answer different questions. What it
       * holds is taken from what was written in it — every element the same
       * type gives `number[]`, a mixture gives the union of them, and a spread
       * gives up, because what it spreads is not known here. */
      TypeId element = TYPE_ERROR;
      bool known = node->as.arrayLiteral.count > 0;
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        TypeId each = checkNode(checker, node->as.arrayLiteral.elements[i]);
        if (node->as.arrayLiteral.elements[i]->type == AST_SPREAD) known = false;
        element = element == TYPE_ERROR ? each : csTypeUnionWith(checker->types, element, each);
      }
      result = known ? csTypeArrayOf(checker->types, element) : TYPE_ARRAY;
      break;
    }

    case AST_CONDITIONAL: {
      checkNode(checker, node->as.conditional.condition);
      TypeId thenType = checkNode(checker, node->as.conditional.thenValue);
      TypeId elseType = checkNode(checker, node->as.conditional.elseValue);
      /* Without union types the result is only known when both arms agree. */
      result = thenType == elseType ? thenType : TYPE_DYNAMIC;
      break;
    }

    case AST_SPREAD:
      checkNode(checker, node->as.spread);
      result = TYPE_DYNAMIC;
      break;

    case AST_DESTRUCTURE:
      checkNode(checker, node->as.destructure.initializer);
      for (int i = 0; i < node->as.destructure.count; i++) {
        checkNode(checker, node->as.destructure.bindings[i].defaultValue);
        /* Element and property types are not modelled, so each binding is
         * dynamic. */
        csTypeDeclareVariable(checker, node->as.destructure.bindings[i].name, node->as.destructure.bindings[i].nameLength, TYPE_DYNAMIC);
      }
      result = TYPE_UNDEFINED;
      break;

    case AST_REGEX_LITERAL: result = TYPE_OBJECT; break;

    case AST_DYNAMIC_IMPORT:
      checkNode(checker, node->as.unary.operand);
      result = TYPE_DYNAMIC; /* a promise, which the lattice does not model */
      break;

    case AST_THIS:
    case AST_NEW_TARGET:
    case AST_SUPER: result = TYPE_DYNAMIC; break;

    /* What a promise resolves to is not modelled, so awaiting one is dynamic.
     * An async function's declared return type describes what it resolves to
     * rather than what calling it produces, so it is not checked either. */
    case AST_AWAIT:
      checkNode(checker, node->as.unary.operand);
      result = TYPE_DYNAMIC;
      break;

      /* Types do not cross a module boundary yet: the checker runs per file and
       * has no record of what another file resolved. An imported binding is
       * dynamic, which is honest rather than merely permissive. */

    default: return false;
  }

  *out = result;
  return true;
}
