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
  /* An unevaluated form is waiting on something, and that something is a
   * variable somewhere inside it. */
  if (composite->kind == COMPOSITE_KEYOF || composite->kind == COMPOSITE_INDEXED || composite->kind == COMPOSITE_MAPPED) return true;
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
    case COMPOSITE_TYPEVAR:
    /* A literal holds text and nothing that could mention a variable. */
    case COMPOSITE_LITERAL: return type;

    /* The computed forms: their pieces are substituted, and then the result is
     * *evaluated* — which is where `Partial<T>` becomes a shape, because this
     * is the moment T stopped being a variable. */
    case COMPOSITE_KEYOF: {
      TypeId subject = substitute(table, composite->inner, params, args, count, depth + 1);
      return csTypeEvaluate(table, csTypeKeyOf(table, subject));
    }

    case COMPOSITE_INDEXED: {
      TypeId subject = substitute(table, composite->inner, params, args, count, depth + 1);
      TypeId key = substitute(table, table->slots[composite->slotStart], params, args, count, depth + 1);
      return csTypeEvaluate(table, csTypeIndexedAccess(table, subject, key));
    }

    case COMPOSITE_MAPPED: {
      TypeId keys = substitute(table, composite->inner, params, args, count, depth + 1);
      TypeId variable = table->slots[composite->slotStart];
      /* The value keeps its own variable — it is bound to each name in turn by
       * the mapping itself, not by this substitution. */
      TypeId value = substitute(table, table->slots[composite->slotStart + 1], params, args, count, depth + 1);
      TypeId source = composite->slotCount > 2 ? substitute(table, table->slots[composite->slotStart + 2], params, args, count, depth + 1) : TYPE_DYNAMIC;
      return csTypeMapped(table, keys, variable, value, composite->optionalMode, composite->readonlyMode, composite->name, composite->nameLength, source);
    }

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
      /* The same generic with the same arguments is the same type, and asking
       * that *first* is what makes a shape that mentions itself work: `set` on
       * a `Map<K, V>` answers a Map, and substituting through it would
       * otherwise build one copy per level until the depth cap stopped it. */
      for (int i = 0; i < table->compositeCount; i++) {
        const CompositeType *candidate = &table->composites[i];
        if (candidate->genericOf != type || candidate->slotCount != count) continue;
        bool same = true;
        for (int j = 0; j < count && same; j++) same = table->slots[candidate->slotStart + j] == args[j];
        if (same) return csTypeCompositeAt(i);
      }

      int sourceStart = composite->memberStart;
      int sourceCount = composite->memberCount;

      TypeId copy = csTypeDeclareInterface(table, composite->name, composite->nameLength);
      if (copy == TYPE_ERROR) return type;

      /* Recorded as an instantiation *before* its members are copied, so that
       * the search above finds it while they are still being built — and so a
       * message can say `Box<number>` rather than a second, unexplained Box. */
      int start;
      CompositeType *made = &table->composites[csTypeCompositeIndex(copy)];
      made->genericOf = type;
      made->open = composite->open;
      if (csTypeTakeSlots(table, args, count, &start)) {
        made->slotStart = start;
        made->slotCount = count;
      }

      for (int i = 0; i < sourceCount; i++) {
        TypeMember member = table->members[sourceStart + i];
        member.type = substitute(table, member.type, params, args, count, depth + 1);
        if (!csTypeAddMember(table, copy, &member)) break;
      }
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
