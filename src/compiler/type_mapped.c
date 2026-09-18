/* type_mapped.c — the types computed from other types.
 *
 * `keyof T`, `T[K]` and `{ [K in keyof T]?: T[K] }` all describe a type in
 * terms of another one, and each is worked out here the moment the other stops
 * being a variable. Inside `type Partial<T> = { [K in keyof T]?: T[K] }` there
 * is no T yet, so the mapped type is *kept* as one and evaluated at
 * `Partial<User>` — which is the same point a generic is instantiated, through
 * the same substitution.
 *
 * That is the whole design: an unevaluated form is a composite like any other,
 * substitution knows how to finish it, and nothing outside this file has to
 * know the difference. Until it is finished it behaves as what the checker
 * could not work out, which is exactly what it is.
 *
 * TypeScript's utility types are built from these three and nothing else, and
 * so are the ones in type_utility.c.
 */
#include <string.h>

#include "compiler/type_internal.h"

/* An unevaluated composite: the pieces it is computed from, kept for later. */
static TypeId unevaluated(TypeTable *table, CompositeKind kind, TypeId subject, const TypeId *slots, int slotCount) {
  for (int i = 0; i < table->compositeCount; i++) {
    const CompositeType *candidate = &table->composites[i];
    if (candidate->kind != kind || candidate->inner != subject || candidate->slotCount != slotCount) continue;
    bool same = true;
    for (int j = 0; j < slotCount && same; j++) same = table->slots[candidate->slotStart + j] == slots[j];
    if (same) return csTypeCompositeAt(i);
  }

  int start = 0;
  if (slotCount > 0 && !csTypeTakeSlots(table, slots, slotCount, &start)) return TYPE_DYNAMIC;

  TypeId id;
  CompositeType *composite = csTypeNewComposite(table, kind, &id);
  if (composite == NULL) return TYPE_DYNAMIC;
  composite->inner = subject;
  composite->slotStart = start;
  composite->slotCount = slotCount;
  return id;
}

/* The names of a shape's members, as a union of literals. An empty shape has
 * no keys at all, which is a union of nothing — `never` in TypeScript, and
 * here the type that says the checker could not work it out. */
TypeId csTypeKeyOf(TypeTable *table, TypeId subject) {
  const CompositeType *shape = csTypeComposite(table, subject);
  if (shape == NULL || shape->kind != COMPOSITE_INTERFACE) {
    /* Not a shape yet — a type variable, or something with no members to
     * name. Kept until it is one. */
    if (shape != NULL && (shape->kind == COMPOSITE_TYPEVAR || shape->kind == COMPOSITE_MAPPED)) {
      return unevaluated(table, COMPOSITE_KEYOF, subject, NULL, 0);
    }
    return TYPE_DYNAMIC;
  }

  TypeId names[16];
  int count = 0;
  for (int i = 0; i < shape->memberCount && count < (int)(sizeof names / sizeof names[0]); i++) {
    const TypeMember *member = &table->members[shape->memberStart + i];
    names[count++] = csTypeLiteral(table, member->name, member->length);
  }
  if (count == 0) return TYPE_DYNAMIC;
  return csTypeUnionOf(table, names, count);
}

/* `T[K]` — what the member named K holds. A union of names answers a union of
 * what they hold, which is what makes `T[keyof T]` the type of any of its
 * values. */
TypeId csTypeIndexedAccess(TypeTable *table, TypeId subject, TypeId key) {
  const CompositeType *shape = csTypeComposite(table, subject);
  if (shape == NULL || shape->kind != COMPOSITE_INTERFACE) {
    TypeId slots[1] = {key};
    if (shape != NULL && (shape->kind == COMPOSITE_TYPEVAR || shape->kind == COMPOSITE_MAPPED)) {
      return unevaluated(table, COMPOSITE_INDEXED, subject, slots, 1);
    }
    /* An array indexed by a number is its element, which is the one indexed
     * access that needs no shape. */
    if (csTypeIsArrayLike(table, subject)) return csTypeElementOf(table, subject);
    return TYPE_DYNAMIC;
  }

  const CompositeType *keyShape = csTypeComposite(table, key);
  if (keyShape != NULL && keyShape->kind == COMPOSITE_TYPEVAR) {
    TypeId slots[1] = {key};
    return unevaluated(table, COMPOSITE_INDEXED, subject, slots, 1);
  }

  /* A union of names: the union of what each of them holds. */
  if (keyShape != NULL && keyShape->kind == COMPOSITE_UNION) {
    TypeId held[16];
    int count = keyShape->slotCount;
    if (count > (int)(sizeof held / sizeof held[0])) return TYPE_DYNAMIC;
    for (int i = 0; i < count; i++) {
      held[i] = csTypeIndexedAccess(table, subject, table->slots[keyShape->slotStart + i]);
    }
    return csTypeUnionOf(table, held, count);
  }

  int length;
  const char *text = csTypeLiteralText(table, key, &length);
  if (text == NULL) {
    /* `T[string]` is any of its members, which is the union of all of them. */
    if (key == TYPE_STRING) return csTypeIndexedAccess(table, subject, csTypeKeyOf(table, subject));
    return TYPE_DYNAMIC;
  }

  const TypeMember *member = csTypeFindMember(table, subject, text, length);
  return member != NULL ? member->type : TYPE_DYNAMIC;
}

/* `{ [K in keys]?: value }`.
 *
 * Evaluated when `keys` is a name or a union of them, and kept when it is not.
 * The variable K is what `value` mentions, and is bound to each name in turn —
 * which is what makes `T[K]` inside one mean "what this member holds". */
TypeId csTypeMapped(TypeTable *table, TypeId keys, TypeId variable, TypeId value, TypeModifier optionalMode, TypeModifier readonlyMode, const char *name,
                    int nameLength, TypeId source) {
  const CompositeType *keyShape = csTypeComposite(table, keys);
  bool isName = keyShape != NULL && keyShape->kind == COMPOSITE_LITERAL;
  bool isNames = keyShape != NULL && keyShape->kind == COMPOSITE_UNION;

  if (!isName && !isNames) {
    /* Not a set of names yet. Kept whole, with the variable and the value in
     * the slots, so substitution can finish it later. */
    /* The shape the keys came from travels with the mapping: `{ [K in keyof
     * T]: … }` keeps what T said about each member unless the mapping says
     * otherwise, and after the keys are evaluated there is no way back to it. */
    TypeId slots[3] = {variable, value, source};
    TypeId id = unevaluated(table, COMPOSITE_MAPPED, keys, slots, 3);
    if (csTypeComposite(table, id) != NULL) {
      CompositeType *made = &table->composites[csTypeCompositeIndex(id)];
      made->optionalMode = optionalMode;
      made->readonlyMode = readonlyMode;
      /* The name the alias gave it, carried so that what it evaluates to can
       * be called `Partial` in a message rather than nothing at all. */
      if (name != NULL && made->name == NULL) {
        made->name = name;
        made->nameLength = nameLength;
      }
    }
    return id;
  }

  TypeId shape = csTypeDeclareInterface(table, name != NULL ? name : "", name != NULL ? nameLength : 0);
  if (shape == TYPE_ERROR) return TYPE_DYNAMIC;

  int count = isName ? 1 : keyShape->slotCount;
  int start = isName ? 0 : keyShape->slotStart;
  for (int i = 0; i < count; i++) {
    TypeId each = isName ? keys : table->slots[start + i];
    int textLength;
    const char *text = csTypeLiteralText(table, each, &textLength);
    if (text == NULL) continue;

    /* The value with K standing for this name: `T[K]` becomes what this member
     * holds, and anything else that mentions K follows the same way. */
    TypeMember member;
    memset(&member, 0, sizeof member);
    member.name = text;
    member.length = textLength;
    member.type = csTypeSubstitute(table, value, &variable, &each, 1);
    member.optional = optionalMode == MODIFIER_ADD;
    member.readonly = readonlyMode == MODIFIER_ADD;

    /* What the source said, when this mapping does not say otherwise:
     * `{ [K in keyof T]: T[K] | null }` keeps T's own optional members
     * optional. */
    if (optionalMode == MODIFIER_KEEP || readonlyMode == MODIFIER_KEEP) {
      const TypeMember *original = csTypeFindMember(table, source, text, textLength);
      if (original != NULL) {
        if (optionalMode == MODIFIER_KEEP) member.optional = original->optional;
        if (readonlyMode == MODIFIER_KEEP) member.readonly = original->readonly;
      }
    }
    csTypeAddMember(table, shape, &member);
  }
  return shape;
}

/* `T extends U ? X : Y`.
 *
 * Evaluated the moment the checked side stops being a variable, and kept until
 * then. What it answers is a *choice*, which is what separates it from the
 * three above: they transform a shape, this one picks between two types by
 * asking the assignability question the checker asks everywhere else.
 *
 * A conditional written over a bare type parameter **distributes**: it is
 * applied to each member of a union separately and the answers joined. That is
 * the whole of why `Exclude<"a" | "b", "a">` is `"b"` — asked of the union as
 * one thing, `"a" | "b"` is not assignable to `"a"` and the answer would be
 * the entire union. */
TypeId csTypeConditional(TypeTable *table, TypeId check, TypeId extends, TypeId whenTrue, TypeId whenFalse, const TypeId *infers, int inferCount) {
  const CompositeType *subject = csTypeComposite(table, check);
  bool unresolved = subject != NULL && (subject->kind == COMPOSITE_TYPEVAR || subject->kind == COMPOSITE_MAPPED || subject->kind == COMPOSITE_KEYOF ||
                                        subject->kind == COMPOSITE_INDEXED || subject->kind == COMPOSITE_CONDITIONAL);

  if (unresolved) {
    /* Nothing to decide yet. Kept whole, with the three other types in the
     * slots and any `infer` names after them, so substitution can finish it
     * later — and remembering that the checked side was written as a variable,
     * because after substitution there is no way back to that fact. */
    TypeId slots[3 + CS_MAX_TYPE_PARAMS];
    slots[0] = extends;
    slots[1] = whenTrue;
    slots[2] = whenFalse;
    int slotCount = 3;
    for (int i = 0; i < inferCount && slotCount < (int)(sizeof slots / sizeof slots[0]); i++) slots[slotCount++] = infers[i];

    TypeId id = unevaluated(table, COMPOSITE_CONDITIONAL, check, slots, slotCount);
    if (csTypeComposite(table, id) != NULL) {
      table->composites[csTypeCompositeIndex(id)].distributes = subject->kind == COMPOSITE_TYPEVAR;
    }
    return id;
  }

  /* No `infer`: the question is exactly assignability, which is what the
   * checker asks everywhere else. */
  if (inferCount <= 0) return csTypeAssignableIn(table, check, extends) ? whenTrue : whenFalse;

  /* With one, the checked type is *matched* against the pattern and each name
   * catches whatever stood where it did — the same structural walk a generic
   * call uses to work out what T was. Matching alone is not a proof, though:
   * it fills what it can and says nothing about the rest. So the pattern is
   * rebuilt with what was caught, and the ordinary assignability question is
   * asked of that. */
  TypeId caught[CS_MAX_TYPE_PARAMS];
  int count = inferCount > CS_MAX_TYPE_PARAMS ? CS_MAX_TYPE_PARAMS : inferCount;
  for (int i = 0; i < count; i++) caught[i] = TYPE_DYNAMIC;

  csTypeInfer(table, extends, check, infers, caught, count);

  TypeId filled = csTypeSubstitute(table, extends, infers, caught, count);
  if (!csTypeAssignableIn(table, check, filled)) return whenFalse;
  return csTypeSubstitute(table, whenTrue, infers, caught, count);
}

/* Finishes an unevaluated type once substitution has made its pieces concrete.
 * Answers the type unchanged when there is still nothing to work out. */
TypeId csTypeEvaluate(TypeTable *table, TypeId type) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL) return type;

  switch (composite->kind) {
    case COMPOSITE_KEYOF: return csTypeKeyOf(table, composite->inner);
    case COMPOSITE_INDEXED: return csTypeIndexedAccess(table, composite->inner, table->slots[composite->slotStart]);
    case COMPOSITE_CONDITIONAL: {
      /* Not distributing here, and that is not an omission. Distribution means
       * binding the checked variable to each member of the union and redoing
       * the *whole* conditional — the arms mention that variable too, and
       * answering them with the union still in place is how `Exclude` comes
       * back as everything it was asked to remove. Only substitution holds the
       * binding, so only substitution can do it; see type_generic.c. */
      TypeId extends = table->slots[composite->slotStart];
      TypeId whenTrue = table->slots[composite->slotStart + 1];
      TypeId whenFalse = table->slots[composite->slotStart + 2];
      return csTypeConditional(table, composite->inner, extends, whenTrue, whenFalse, &table->slots[composite->slotStart + 3], composite->slotCount - 3);
    }

    case COMPOSITE_MAPPED: {
      TypeId variable = table->slots[composite->slotStart];
      TypeId value = table->slots[composite->slotStart + 1];
      TypeId source = composite->slotCount > 2 ? table->slots[composite->slotStart + 2] : TYPE_DYNAMIC;
      return csTypeMapped(table, composite->inner, variable, value, composite->optionalMode, composite->readonlyMode, composite->name, composite->nameLength, source);
    }
    default: return type;
  }
}
