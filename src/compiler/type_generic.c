/* type_generic.c — substitution, instantiation, and what a call site infers.
 *
 * A generic declaration is checked **once**, with its type variables standing
 * for themselves: inside `function first<T>(items: T[]): T` nothing is known
 * about a T beyond being one, which is what makes the body true for every
 * instantiation at once.
 *
 * Everything else happens where the generic is used. `Box<number>` substitutes
 * through a declaration; a call substitutes through a signature after matching
 * the arguments against the parameters to work out what each variable stands
 * for there. Matching is structural and one-way, and the first answer wins — a
 * later, different one is a mismatch the ordinary check reports, with both
 * types named, rather than something this file tries to reconcile.
 */
#include <string.h>

#include "compiler/type_internal.h"

/* --- generics ------------------------------------------------------------ */

static bool mentions(const TypeTable *table, TypeId type, int depth) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL || depth > 6) return false;
  if (composite->kind == COMPOSITE_TYPEVAR) return true;
  if (mentions(table, composite->inner, depth + 1)) return true;
  for (int i = 0; i < composite->slotCount; i++) {
    if (mentions(table, table->slots[composite->slotStart + i], depth + 1)) return true;
  }
  for (int i = 0; i < composite->memberCount; i++) {
    if (mentions(table, table->members[composite->memberStart + i].type, depth + 1)) return true;
  }
  return false;
}

bool csTypeMentionsTypeVar(const TypeTable *table, TypeId type) {
  return mentions(table, type, 0);
}

static TypeId substitute(TypeTable *table, TypeId type, const TypeId *params, const TypeId *args, int count, int depth) {
  if (depth > 6 || !csTypeIsComposite(type)) return type;
  for (int i = 0; i < count; i++) {
    if (type == params[i]) return args[i];
  }
  if (!mentions(table, type, 0)) return type;

  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL) return type;

  switch (composite->kind) {
    case COMPOSITE_TYPEVAR: return type;

    case COMPOSITE_ARRAY: return csTypeArrayOf(table, substitute(table, composite->inner, params, args, count, depth + 1));

    case COMPOSITE_UNION: {
      TypeId members[16];
      int memberCount = composite->slotCount;
      if (memberCount > (int)(sizeof members / sizeof members[0])) return type;
      for (int i = 0; i < memberCount; i++) {
        members[i] = substitute(table, table->slots[composite->slotStart + i], params, args, count, depth + 1);
      }
      return csTypeUnionOf(table, members, memberCount);
    }

    case COMPOSITE_FUNCTION: {
      TypeId slots[16];
      int slotCount = composite->slotCount;
      if (slotCount > (int)(sizeof slots / sizeof slots[0])) return type;
      for (int i = 0; i < slotCount; i++) {
        slots[i] = substitute(table, table->slots[composite->slotStart + i], params, args, count, depth + 1);
      }
      TypeId result = substitute(table, composite->inner, params, args, count, depth + 1);
      return csTypeFunctionOf(table, slots, slotCount, composite->requiredCount, composite->hasRest, result);
    }

    case COMPOSITE_INTERFACE: {
      /* A new shape with the arguments put through it, recorded as an
       * instantiation of the one it came from so that a message can say
       * `Box<number>` rather than a second, unexplained `Box`. */
      int sourceStart = composite->memberStart;
      int sourceCount = composite->memberCount;

      TypeId copy = csTypeDeclareInterface(table, composite->name, composite->nameLength);
      if (copy == TYPE_ERROR) return type;

      for (int i = 0; i < sourceCount; i++) {
        TypeMember member = table->members[sourceStart + i];
        member.type = substitute(table, member.type, params, args, count, depth + 1);
        if (!csTypeAddMember(table, copy, &member)) break;
      }

      int start;
      CompositeType *made = &table->composites[csTypeCompositeIndex(copy)];
      if (csTypeTakeSlots(table, args, count, &start)) {
        made->slotStart = start;
        made->slotCount = count;
      }
      made->genericOf = type;
      return copy;
    }
  }
  return type;
}

TypeId csTypeSubstitute(TypeTable *table, TypeId type, const TypeId *params, const TypeId *args, int count) {
  if (count <= 0) return type;
  return substitute(table, type, params, args, count, 0);
}

TypeId csTypeInstantiate(TypeTable *table, TypeId generic, const TypeId *args, int argCount) {
  const CompositeType *composite = csTypeComposite(table, generic);
  if (composite == NULL || composite->typeParamCount == 0) return generic;

  TypeId params[CS_MAX_TYPE_PARAMS];
  TypeId filled[CS_MAX_TYPE_PARAMS];
  int count = composite->typeParamCount;
  for (int i = 0; i < count; i++) {
    params[i] = composite->typeParams[i];
    /* A missing argument is one the caller did not say and the checker could
     * not work out, which is exactly DYNAMIC. */
    filled[i] = i < argCount ? args[i] : TYPE_DYNAMIC;
  }
  return csTypeSubstitute(table, generic, params, filled, count);
}

/* Matching is structural and one-way: it walks the parameter's type and the
 * argument's together, and every time the parameter is a variable, the
 * argument says what that variable must be. The first answer wins — a later,
 * different one is a mismatch the ordinary check reports. */
static void infer(const TypeTable *table, TypeId parameter, TypeId argument, const TypeId *params, TypeId *bindings, int count, int depth) {
  if (depth > 6 || argument == TYPE_ERROR) return;

  for (int i = 0; i < count; i++) {
    if (parameter != params[i]) continue;
    if (bindings[i] == TYPE_DYNAMIC) bindings[i] = argument;
    return;
  }

  const CompositeType *want = csTypeComposite(table, parameter);
  const CompositeType *given = csTypeComposite(table, argument);
  if (want == NULL) return;

  if (want->kind == COMPOSITE_ARRAY) {
    /* `T[]` against `number[]` says T is a number; against a bare `array` it
     * says nothing, which leaves T to another argument or to DYNAMIC. */
    if (given != NULL && given->kind == COMPOSITE_ARRAY) infer(table, want->inner, given->inner, params, bindings, count, depth + 1);
    return;
  }

  if (want->kind == COMPOSITE_FUNCTION && given != NULL && given->kind == COMPOSITE_FUNCTION) {
    int shared = want->slotCount < given->slotCount ? want->slotCount : given->slotCount;
    for (int i = 0; i < shared; i++) {
      infer(table, table->slots[want->slotStart + i], table->slots[given->slotStart + i], params, bindings, count, depth + 1);
    }
    infer(table, want->inner, given->inner, params, bindings, count, depth + 1);
    return;
  }

  if (want->kind == COMPOSITE_INTERFACE && given != NULL && given->kind == COMPOSITE_INTERFACE) {
    for (int i = 0; i < want->memberCount; i++) {
      const TypeMember *member = &table->members[want->memberStart + i];
      const TypeMember *match = csTypeFindMember(table, argument, member->name, member->length);
      if (match == NULL) continue;
      infer(table, member->type, match->type, params, bindings, count, depth + 1);
    }
  }
}

void csTypeInfer(const TypeTable *table, TypeId parameter, TypeId argument, const TypeId *params, TypeId *bindings, int count) {
  infer(table, parameter, argument, params, bindings, count, 0);
}
