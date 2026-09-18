/* type_utility.c — the utility types, each one a mapping written out.
 *
 * TypeScript defines these in a declaration file, as mapped types over the
 * argument. Here they are the same mappings applied directly, for one reason:
 * a mapping needs a shape to map over, and these are asked for by name at the
 * point the shape is known — so there is nothing to keep unevaluated and no
 * prelude to parse.
 *
 *     Partial<T>      every member optional
 *     Required<T>     every member not
 *     Readonly<T>     every member unwritable
 *     Pick<T, K>      only the members named in K
 *     Omit<T, K>      every member except those
 *     Record<K, V>    one member per name in K, each holding V
 *     Exclude<T, U>   the members of T not assignable to U
 *     Extract<T, U>   the members of T that are
 *     NonNullable<T>  T without null and undefined
 *
 * Each answers a shape named after itself, so a message says `Partial` rather
 * than describing what it produced. What each refuses is what its argument
 * refuses: `Pick<User, "nope">` is an error because "nope" is not a key of
 * User, and that check lives here rather than in the parser because only here
 * are both sides known.
 */
#include <string.h>

#include "compiler/type_internal.h"

/* True when `name` is one of the names in `keys` — a literal or a union. */
static bool namesInclude(const TypeTable *table, TypeId keys, const char *name, int length) {
  const CompositeType *composite = csTypeComposite(table, keys);
  if (composite == NULL) return false;

  if (composite->kind == COMPOSITE_LITERAL) return csTypeNameMatches(name, length, composite->name);
  if (composite->kind != COMPOSITE_UNION) return false;
  for (int i = 0; i < composite->slotCount; i++) {
    const CompositeType *member = csTypeComposite(table, table->slots[composite->slotStart + i]);
    if (member == NULL || member->kind != COMPOSITE_LITERAL) continue;
    if (csTypeNameMatches(name, length, member->name)) return true;
  }
  return false;
}

/* The shared shape of the five that map over a shape's own members. */
static TypeId mapMembers(TypeTable *table, const char *name, int nameLength, TypeId subject, TypeId keys, bool keep, TypeModifier optionalMode,
                         TypeModifier readonlyMode) {
  const CompositeType *shape = csTypeComposite(table, subject);
  if (shape == NULL || shape->kind != COMPOSITE_INTERFACE) return TYPE_DYNAMIC;

  TypeId made = csTypeDeclareInterface(table, name, nameLength);
  if (made == TYPE_ERROR) return TYPE_DYNAMIC;

  int start = shape->memberStart;
  int count = shape->memberCount;
  for (int i = 0; i < count; i++) {
    TypeMember member = table->members[start + i];
    if (keys != TYPE_ERROR && namesInclude(table, keys, member.name, member.length) != keep) continue;
    if (optionalMode != MODIFIER_KEEP) member.optional = optionalMode == MODIFIER_ADD;
    if (readonlyMode != MODIFIER_KEEP) member.readonly = readonlyMode == MODIFIER_ADD;
    csTypeAddMember(table, made, &member);
  }
  return made;
}

/* `Record<K, V>` — one member per name in K, each holding V. With a `string`
 * key there are no names to make members of, so the shape takes an index
 * instead: every key answers V, which is what a dictionary is. */
static TypeId makeRecord(TypeTable *table, TypeId keys, TypeId held) {
  TypeId made = csTypeDeclareInterface(table, "Record", 6);
  if (made == TYPE_ERROR) return TYPE_DYNAMIC;

  const CompositeType *shape = csTypeComposite(table, keys);
  bool isName = shape != NULL && shape->kind == COMPOSITE_LITERAL;
  bool isNames = shape != NULL && shape->kind == COMPOSITE_UNION;

  if (!isName && !isNames) {
    CompositeType *record = &table->composites[csTypeCompositeIndex(made)];
    record->indexValue = held;
    return made;
  }

  int count = isName ? 1 : shape->slotCount;
  for (int i = 0; i < count; i++) {
    TypeId each = isName ? keys : table->slots[shape->slotStart + i];
    int length;
    const char *text = csTypeLiteralText(table, each, &length);
    if (text == NULL) continue;

    TypeMember member;
    memset(&member, 0, sizeof member);
    member.name = text;
    member.length = length;
    member.type = held;
    member.optional = false;
    member.readonly = false;
    csTypeAddMember(table, made, &member);
  }
  return made;
}

/* `Exclude<T, U>` and `Extract<T, U>` — the members of T that are, or are not,
 * assignable to U.
 *
 * Written out here for the same reason the five above are: they are asked for
 * by name at the point both sides are known, so there is nothing to keep
 * unevaluated. What they *mean* is a conditional — `T extends U ? never : T` —
 * and a program can write that now and get the same answer, member by member,
 * because a distributing conditional is exactly this loop. */
static TypeId filterUnion(TypeTable *table, TypeId subject, TypeId against, bool keepMatching) {
  const CompositeType *shape = csTypeComposite(table, subject);
  if (shape == NULL || shape->kind != COMPOSITE_UNION) {
    /* Not a union: one member, kept or dropped whole. Dropping it leaves
     * nothing, which is what `never` is. */
    bool matches = csTypeAssignableIn(table, subject, against);
    return matches == keepMatching ? subject : TYPE_NEVER;
  }

  TypeId kept[16];
  int count = shape->slotCount;
  if (count > (int)(sizeof kept / sizeof kept[0])) return TYPE_DYNAMIC;
  for (int i = 0; i < count; i++) {
    TypeId member = table->slots[shape->slotStart + i];
    bool matches = csTypeAssignableIn(table, member, against);
    kept[i] = matches == keepMatching ? member : TYPE_NEVER;
  }
  /* The dropped members are `never`, and a `never` is the identity of a
   * union — so they are gone by the time this answers. */
  return csTypeUnionOf(table, kept, count);
}

/* Applies the utility of that name, or answers TYPE_ERROR when the name is not
 * one — which is how the caller knows to carry on looking. `wanted` says how
 * many arguments the name takes, so the parser can report a miscount the same
 * way it does for any other generic. */
TypeId csTypeUtility(TypeTable *table, const char *name, int length, const TypeId *args, int argCount, int *wanted) {
  struct {
    const char *name;
    int arity;
  } known[] = {
      {"Partial", 1}, {"Required", 1}, {"Readonly", 1}, {"Pick", 2}, {"Omit", 2}, {"Record", 2}, {"Exclude", 2}, {"Extract", 2}, {"NonNullable", 1},
  };

  *wanted = 0;
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (!csTypeNameMatches(name, length, known[i].name)) continue;
    *wanted = known[i].arity;
    break;
  }
  if (*wanted == 0) return TYPE_ERROR;
  if (argCount != *wanted) return TYPE_DYNAMIC;

  if (csTypeNameMatches(name, length, "Partial")) {
    return mapMembers(table, "Partial", 7, args[0], TYPE_ERROR, true, MODIFIER_ADD, MODIFIER_KEEP);
  }
  if (csTypeNameMatches(name, length, "Required")) {
    return mapMembers(table, "Required", 8, args[0], TYPE_ERROR, true, MODIFIER_REMOVE, MODIFIER_KEEP);
  }
  if (csTypeNameMatches(name, length, "Readonly")) {
    return mapMembers(table, "Readonly", 8, args[0], TYPE_ERROR, true, MODIFIER_KEEP, MODIFIER_ADD);
  }
  if (csTypeNameMatches(name, length, "Pick")) {
    return mapMembers(table, "Pick", 4, args[0], args[1], true, MODIFIER_KEEP, MODIFIER_KEEP);
  }
  if (csTypeNameMatches(name, length, "Omit")) {
    return mapMembers(table, "Omit", 4, args[0], args[1], false, MODIFIER_KEEP, MODIFIER_KEEP);
  }
  if (csTypeNameMatches(name, length, "Exclude")) return filterUnion(table, args[0], args[1], false);
  if (csTypeNameMatches(name, length, "Extract")) return filterUnion(table, args[0], args[1], true);
  if (csTypeNameMatches(name, length, "NonNullable")) {
    TypeId absent[2] = {TYPE_NULL, TYPE_UNDEFINED};
    return filterUnion(table, args[0], csTypeUnionOf(table, absent, 2), false);
  }
  return makeRecord(table, args[0], args[1]);
}
