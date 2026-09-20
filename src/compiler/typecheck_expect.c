/* typecheck_expect.c — the two checks that need to know what was *expected*.
 *
 * Everywhere else the checker works bottom-up: it asks what an expression is
 * and reports where that does not fit. These two go the other way, because
 * two things can only be checked against the type they are being given to:
 *
 *   an object literal against an interface — its members are written down at
 *   the point it is handed over, and nowhere afterwards
 *
 *   a call through a value whose *type* is a function — the signature is in
 *   the type rather than in a declaration, which is what makes a callback
 *   checkable at all
 */
#include <string.h>

#include "compiler/typecheck_internal.h"

/* An object literal given to an interface, checked member by member.
 *
 * This is the one place a shape is proved rather than assumed, and it is
 * deliberately the *literal* that is checked rather than the type `object`: a
 * literal is written at the point it is given away, so its members are still
 * visible here. A value already typed `object` has lost them, and calling it a
 * Point later would be a claim nothing checked.
 *
 * Excess members are refused, as TypeScript refuses them on a fresh literal: a
 * name the interface does not have is nearly always a misspelling of one it
 * does. Answers the type the literal should be treated as having.
 */
TypeId csTypeCheckShape(Checker *checker, AstNode *value, TypeId expected) {
  if (value == NULL) return expected;

  /* `[{ x: 1, y: 2 }]` given to a `Point[]` — the elements are literals too,
   * and each is proved against what the array holds. Without this the array
   * would be an `object[]`, which satisfies nothing shaped. */
  /* `const p: [number, string] = [1, "one"]` — positionally, because that is
   * what a tuple is. The count has to match: a tuple is the claim that there
   * are exactly this many, and a literal of the wrong length is not one. */
  if (value->type == AST_ARRAY_LITERAL && csTypeIs(checker->types, expected, COMPOSITE_TUPLE)) {
    int wanted = csTypeTupleLength(checker->types, expected);
    if (wanted != value->as.arrayLiteral.count) return value->resolvedType;

    TypeId elements[CS_MAX_TYPE_PARAMS * 4];
    if (wanted > (int)(sizeof elements / sizeof elements[0])) return value->resolvedType;
    for (int i = 0; i < wanted; i++) {
      AstNode *item = value->as.arrayLiteral.elements[i];
      if (item->type == AST_SPREAD) return value->resolvedType;
      TypeId want = csTypeTupleElement(checker->types, expected, i);
      TypeId given = csTypeCheckShape(checker, item, want);
      if (!csTypeAssignableIn(checker->types, given, want)) return value->resolvedType;
      elements[i] = want;
    }
    return csTypeTupleOf(checker->types, elements, wanted);
  }

  if (value->type == AST_ARRAY_LITERAL && csTypeIs(checker->types, expected, COMPOSITE_ARRAY)) {
    TypeId element = csTypeElementOf(checker->types, expected);
    bool ok = true;
    for (int i = 0; i < value->as.arrayLiteral.count; i++) {
      AstNode *item = value->as.arrayLiteral.elements[i];
      if (item->type == AST_SPREAD) return value->resolvedType;
      TypeId given = csTypeCheckShape(checker, item, element);
      if (csTypeAssignableIn(checker->types, given, element)) continue;
      ok = false;
    }
    return ok ? expected : value->resolvedType;
  }

  /* `const role: Role = "admin"` — the text is right there, so it is compared
   * against what was asked for rather than widened to `string` first. This is
   * the same rule as an object literal against a shape: a literal is proved
   * where it is written, because that is the only place it can be. */
  /* `-1` is a negation of a literal rather than a literal, and the only way a
   * negative one can be written. Taken here so that `const s: -1 | 1 = -1`
   * reads the way it is written. */
  if (value->type == AST_UNARY && value->as.unary.op == UNARY_NEGATE && value->as.unary.operand != NULL && value->as.unary.operand->type == AST_NUMBER_LITERAL) {
    TypeId literal = csTypeNumberLiteral(checker->types, -value->as.unary.operand->as.number);
    return csTypeAssignableIn(checker->types, literal, expected) ? literal : value->resolvedType;
  }

  if (value->type == AST_NUMBER_LITERAL) {
    TypeId literal = csTypeNumberLiteral(checker->types, value->as.number);
    return csTypeAssignableIn(checker->types, literal, expected) ? literal : value->resolvedType;
  }

  if (value->type == AST_STRING_LITERAL) {
    TypeId literal = csTypeLiteral(checker->types, value->as.string.chars, value->as.string.length);
    return csTypeAssignableIn(checker->types, literal, expected) ? literal : value->resolvedType;
  }

  if (value->type != AST_OBJECT_LITERAL) return value->resolvedType;

  const CompositeType *required = csTypeComposite(checker->types, expected);
  if (required == NULL || required->kind != COMPOSITE_INTERFACE) return value->resolvedType;

  /* A spread, an accessor or a computed key hides what the literal holds, so
   * there is nothing to prove either way and the annotation is taken at its
   * word. */
  for (int i = 0; i < value->as.objectLiteral.count; i++) {
    if (value->as.objectLiteral.kinds[i] != OBJECT_ENTRY_VALUE) return expected;
    if (value->as.objectLiteral.keys[i] == NULL) return expected;
    if (value->as.objectLiteral.keys[i]->type != AST_STRING_LITERAL) return expected;
  }

  bool ok = true;
  for (int i = 0; i < value->as.objectLiteral.count; i++) {
    AstNode *key = value->as.objectLiteral.keys[i];
    const TypeMember *member = csTypeFindMember(checker->types, expected, key->as.string.chars, key->as.string.length);

    /* A shape with an index signature names no members and accepts every key,
     * so what is checked is what each one holds. */
    TypeId anyKey = csTypeIndexValue(checker->types, expected);
    if (member == NULL && anyKey != TYPE_ERROR) {
      TypeId given = csTypeCheckShape(checker, value->as.objectLiteral.values[i], anyKey);
      if (csTypeAssignableIn(checker->types, given, anyKey)) continue;
      csTypeError(checker, value->line, "member '%.*s' is %s but %s holds %s", key->as.string.length, key->as.string.chars, csTypeNameIn(checker->types, given),
                  csTypeNameIn(checker->types, expected), csTypeNameIn(checker->types, anyKey));
      ok = false;
      continue;
    }

    if (member == NULL) {
      csTypeError(checker, value->line, "%s has no member '%.*s'", csTypeNameIn(checker->types, expected), key->as.string.length, key->as.string.chars);
      ok = false;
      continue;
    }
    /* A member that is itself an interface takes a literal of its own, so the
     * check recurses into nested shapes. */
    TypeId given = csTypeCheckShape(checker, value->as.objectLiteral.values[i], member->type);
    if (csTypeAssignableIn(checker->types, given, member->type)) continue;
    csTypeError(checker, value->line, "member '%.*s' is %s but %s declares it %s", key->as.string.length, key->as.string.chars, csTypeNameIn(checker->types, given),
                csTypeNameIn(checker->types, expected), csTypeNameIn(checker->types, member->type));
    ok = false;
  }

  for (int i = 0; i < required->memberCount; i++) {
    const TypeMember *member = &checker->types->members[required->memberStart + i];
    if (member->optional) continue;
    bool present = false;
    for (int j = 0; j < value->as.objectLiteral.count && !present; j++) {
      AstNode *key = value->as.objectLiteral.keys[j];
      present = key->as.string.length == member->length && memcmp(key->as.string.chars, member->name, (size_t)member->length) == 0;
    }
    if (present) continue;
    csTypeError(checker, value->line, "%s needs a member '%.*s' and this has none", csTypeNameIn(checker->types, expected), member->length, member->name);
    ok = false;
  }

  return ok ? expected : TYPE_ERROR;
}

/* A call through a value whose *type* is a function — a parameter annotated
 * `(n: number) => string`, an interface's method, a variable holding one.
 *
 * The signature is in the type rather than in a declaration the checker can
 * see, which is what makes a callback checkable at all: before function types
 * existed, everything passed as `Function` was checked only by the runtime,
 * and the library is mostly higher-order. */
TypeId csTypeCheckCallThrough(Checker *checker, AstNode *node, TypeId functionType) {
  const CompositeType *signature = csTypeComposite(checker->types, functionType);
  if (signature == NULL) return TYPE_DYNAMIC;

  bool hasSpread = false;
  for (int i = 0; i < node->as.call.argCount; i++) {
    if (node->as.call.arguments[i]->type == AST_SPREAD) hasSpread = true;
  }

  int given = node->as.call.argCount;
  bool tooMany = !signature->hasRest && given > signature->slotCount;
  if (!hasSpread && (given < signature->requiredCount || tooMany)) {
    if (signature->requiredCount == signature->slotCount) {
      csTypeError(checker, node->line, "this takes %d argument%s and was given %d", signature->slotCount, signature->slotCount == 1 ? "" : "s", given);
    } else {
      csTypeError(checker, node->line, "this takes between %d and %d arguments and was given %d", signature->requiredCount, signature->slotCount, given);
    }
    return signature->inner;
  }

  for (int i = 0; i < given && !hasSpread; i++) {
    int at = i;
    if (at >= signature->slotCount) {
      if (!signature->hasRest) break;
      at = signature->slotCount - 1;
    }
    TypeId want = checker->types->slots[signature->slotStart + at];
    TypeId have = csTypeCheckShape(checker, node->as.call.arguments[i], want);
    if (csTypeAssignableIn(checker->types, have, want)) continue;
    /* Named where the callee has a name — an imported function is the common
     * case, and "argument 1 of 'length'" beats "argument 1 of this". */
    if (node->as.call.callee->type == AST_IDENTIFIER) {
      csTypeError(checker, node->line, "argument %d of '%.*s' is %s but the parameter is %s", i + 1, node->as.call.callee->as.identifier.length,
                  node->as.call.callee->as.identifier.name, csTypeNameIn(checker->types, have), csTypeNameIn(checker->types, want));
    } else {
      csTypeError(checker, node->line, "argument %d is %s but this takes %s", i + 1, csTypeNameIn(checker->types, have), csTypeNameIn(checker->types, want));
    }
  }
  return signature->inner;
}

/* True when `against` is that kind of literal, or a union with one in it. */
static bool expectsLiteral(Checker *checker, TypeId against, CompositeKind kind) {
  if (csTypeIs(checker->types, against, kind)) return true;
  const CompositeType *composite = csTypeComposite(checker->types, against);
  if (composite == NULL || composite->kind != COMPOSITE_UNION) return false;
  for (int i = 0; i < composite->slotCount; i++) {
    if (csTypeIs(checker->types, checker->types->slots[composite->slotStart + i], kind)) return true;
  }
  return false;
}

/* The literal type a value written in the source has, when the thing it is
 * being compared against is made of literals. Anything else keeps the type it
 * already had — a string stays a string and a number stays a number, which is
 * what almost every comparison wants.
 *
 * A written-out value is the only thing this applies to. `let n = 1` gives `n`
 * the type number, and passing it where `0 | 1` is wanted is refused: what the
 * variable holds could have changed since, and only the literal in the source
 * is a value the checker can still see. */
TypeId csTypeRefineLiteral(Checker *checker, AstNode *node, TypeId against) {
  if (node == NULL) return TYPE_DYNAMIC;

  if (node->type == AST_STRING_LITERAL) {
    if (!expectsLiteral(checker, against, COMPOSITE_LITERAL)) return node->resolvedType;
    return csTypeLiteral(checker->types, node->as.string.chars, node->as.string.length);
  }

  if (node->type == AST_NUMBER_LITERAL) {
    if (!expectsLiteral(checker, against, COMPOSITE_NUMBER_LITERAL)) return node->resolvedType;
    return csTypeNumberLiteral(checker->types, node->as.number);
  }

  return node->resolvedType;
}
