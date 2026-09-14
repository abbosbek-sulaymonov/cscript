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
    csTypeError(checker, node->line, "argument %d is %s but this takes %s", i + 1, csTypeNameIn(checker->types, have), csTypeNameIn(checker->types, want));
  }
  return signature->inner;
}
