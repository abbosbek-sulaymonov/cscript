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

      /* Property and index targets have no declared type to check against,
       * with one exception: a shape may say a member is `readonly`, and
       * writing through one is the whole of what that word forbids. */
      if (target->type != AST_IDENTIFIER) {
        checkNode(checker, target);
        if (target->type == AST_PROPERTY) {
          TypeId owner = target->as.property.object->resolvedType;
          const TypeMember *member = csTypeFindMember(checker->types, owner, target->as.property.name, target->as.property.length);
          if (member != NULL && member->readonly) {
            csTypeError(checker, node->line, "'%.*s' is readonly on %s", target->as.property.length, target->as.property.name, csTypeNameIn(checker->types, owner));
          }
        }
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

      /* `<T extends Named>` — inside the declaration a `T` is whatever it was
       * constrained to, as far as reading a member goes. That is the whole of
       * what a constraint buys a body: without one nothing is known about a
       * type variable, which is why an unconstrained `T.name` is still a
       * mistake. */
      object = csTypeThroughConstraint(checker->types, object);

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
        /* `Record<string, number>` names no members and answers a number for
         * every key, which is what a dictionary is. */
        TypeId anyKey = csTypeIndexValue(checker->types, object);
        if (member == NULL && anyKey != TYPE_ERROR) {
          result = anyKey;
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

    case AST_CALL: result = csTypeCheckCall(checker, node); break;

    case AST_INDEX: {
      TypeId target = checkNode(checker, node->as.index.target);
      target = csTypeThroughConstraint(checker->types, target);
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
      TypeId indexed = csTypeIndexValue(checker->types, target);
      if (indexed != TYPE_ERROR) {
        result = indexed;
        break;
      }

      /* `p[0]` on a tuple is that element, exactly — which is the whole of
       * what a tuple is for. Only where the position is written out: an index
       * the checker cannot read answers any of them, which is what
       * csTypeElementOf gives below. */
      if (csTypeIs(checker->types, target, COMPOSITE_TUPLE) && node->as.index.index->type == AST_NUMBER_LITERAL) {
        double at = node->as.index.index->as.number;
        TypeId element = at == (double)(int)at ? csTypeTupleElement(checker->types, target, (int)at) : TYPE_ERROR;
        if (element != TYPE_ERROR) {
          result = element;
          break;
        }
        csTypeError(checker, node->line, "%s has no element %g", csTypeNameIn(checker->types, target), at);
        result = TYPE_ERROR;
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

      /* `typeof x === "string" ? fromString(x) : x` — a test narrows its arms
       * here exactly as it narrows the branches of an `if`. It is the shape a
       * one-line guard takes, and leaving it out sent every such line back to
       * the runtime. */
#define NARROWED_AT_ONCE 8
      Variable *narrowed[NARROWED_AT_ONCE];
      TypeId saved[NARROWED_AT_ONCE];

      int count = csTypeNarrowAll(checker, node->as.conditional.condition, true, narrowed, saved, NARROWED_AT_ONCE);
      TypeId thenType = checkNode(checker, node->as.conditional.thenValue);
      for (int i = 0; i < count; i++) narrowed[i]->type = saved[i];

      count = csTypeNarrowAll(checker, node->as.conditional.condition, false, narrowed, saved, NARROWED_AT_ONCE);
      TypeId elseType = checkNode(checker, node->as.conditional.elseValue);
      for (int i = 0; i < count; i++) narrowed[i]->type = saved[i];
#undef NARROWED_AT_ONCE

      /* One of the two, which is what a union is for. */
      result = csTypeUnionWith(checker->types, thenType, elseType);
      break;
    }

    case AST_SPREAD:
      checkNode(checker, node->as.spread);
      result = TYPE_DYNAMIC;
      break;

    case AST_DESTRUCTURE: {
      TypeId source = checkNode(checker, node->as.destructure.initializer);

      /* What each binding takes from what it was given.
       *
       * An array pattern over a *tuple* is the case this can answer exactly:
       * position 0 holds what the tuple says position 0 holds. An object
       * pattern over a shape is the other, by name. Anything else is a read
       * the checker cannot follow, and stays what it has always been. */
      bool fromTuple = !node->as.destructure.isObject && csTypeIs(checker->types, source, COMPOSITE_TUPLE);
      bool fromShape = node->as.destructure.isObject && csTypeIs(checker->types, source, COMPOSITE_INTERFACE);

      for (int i = 0; i < node->as.destructure.count; i++) {
        const AstBinding *binding = &node->as.destructure.bindings[i];
        checkNode(checker, binding->defaultValue);

        TypeId held = TYPE_DYNAMIC;
        if (fromTuple && !binding->isRest && binding->pattern == NULL) {
          TypeId element = csTypeTupleElement(checker->types, source, i);
          if (element != TYPE_ERROR) held = element;
        } else if (fromShape && !binding->isRest && binding->pattern == NULL) {
          const char *key = binding->key != NULL ? binding->key : binding->name;
          int keyLength = binding->key != NULL ? binding->keyLength : binding->nameLength;
          const TypeMember *member = csTypeFindMember(checker->types, source, key, keyLength);
          /* An optional member may not be there, and the binding holds
           * undefined when it is not — which is what its type has to say. */
          if (member != NULL) held = member->optional ? csTypeUnionWith(checker->types, member->type, TYPE_UNDEFINED) : member->type;
        }

        csTypeDeclareVariable(checker, binding->name, binding->nameLength, held);
      }
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_REGEX_LITERAL: result = TYPE_OBJECT; break;

    case AST_DYNAMIC_IMPORT:
      checkNode(checker, node->as.unary.operand);
      result = TYPE_DYNAMIC; /* a promise, which the lattice does not model */
      break;

    case AST_THIS:
    case AST_NEW_TARGET:
    case AST_SUPER: result = TYPE_DYNAMIC; break;

    /* `await` unwraps a `Promise<T>` to its T, which is the whole reason the
     * built-in generic is worth having. Awaiting anything else answers what it
     * already was: `await 1` is 1, as it is in JavaScript. */
    case AST_AWAIT: {
      TypeId awaited = checkNode(checker, node->as.unary.operand);
      TypeId resolves;
      result = csTypeIsPromise(checker->types, awaited, &resolves) ? resolves : awaited;
      break;
    }

      /* Types do not cross a module boundary yet: the checker runs per file and
       * has no record of what another file resolved. An imported binding is
       * dynamic, which is honest rather than merely permissive. */

    default: return false;
  }

  *out = result;
  return true;
}
