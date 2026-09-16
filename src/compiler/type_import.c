/* type_import.c — re-interning a type from one file's table into another's.
 *
 * What a module exports is described in the table its own file built, and that
 * table outlives the parse for exactly this reason. Bringing a type across is
 * not inference and not a second pass: it is rebuilding the same type here,
 * piece by piece, and it works because the whole relation is structural — a
 * shape written in two files is one type, with neither importing the other's
 * name.
 *
 * Two things are rebuilt rather than copied. An instantiation — `Map<string,
 * number>` — is made again from the generic behind it, so that this file's
 * Map and that file's Map are the same type rather than two that merely look
 * alike. A type variable is erased: a generic is instantiated at its call
 * site, and an imported binding has no call site here for the checker to read.
 */
#include <string.h>

#include "compiler/type_internal.h"

/* An interface already in `dest` describing exactly this shape, or TYPE_ERROR.
 * Two files that declare the same shape should end up with one type, or every
 * import would grow the table by a copy nobody can tell from the original. */
static TypeId matchingInterface(const TypeTable *dest, const TypeTable *src, const CompositeType *wanted) {
  for (int i = 0; i < dest->compositeCount; i++) {
    const CompositeType *candidate = &dest->composites[i];
    if (candidate->kind != COMPOSITE_INTERFACE) continue;
    if (candidate->memberCount != wanted->memberCount) continue;
    /* A generic and one of its instantiations share a name and their member
     * names, and are not the same type: `Map<K, V>` would otherwise answer for
     * the `Map<string, number>` being imported, and its `get` would still take
     * a K. */
    if (candidate->typeParamCount != wanted->typeParamCount) continue;
    if ((candidate->genericOf != TYPE_DYNAMIC) != (wanted->genericOf != TYPE_DYNAMIC)) continue;
    if (!csTypeNameMatches(wanted->name, wanted->nameLength, candidate->name)) continue;

    bool same = true;
    for (int j = 0; j < wanted->memberCount && same; j++) {
      const TypeMember *want = &src->members[wanted->memberStart + j];
      const TypeMember *have = &dest->members[candidate->memberStart + j];
      same = csTypeNameMatches(want->name, want->length, have->name) && want->optional == have->optional;
    }
    if (same) return csTypeCompositeAt(i);
  }
  return TYPE_ERROR;
}

static TypeId importType(TypeTable *dest, const TypeTable *src, TypeId type, int depth) {
  if (!csTypeIsComposite(type) || depth > 6) return csTypeIsComposite(type) ? TYPE_DYNAMIC : type;

  const CompositeType *composite = csTypeComposite(src, type);
  if (composite == NULL) return TYPE_DYNAMIC;

  switch (composite->kind) {
    /* Erased: a generic is instantiated at its call site, and an imported
     * binding has no call site here for the checker to read. */
    case COMPOSITE_TYPEVAR: return TYPE_DYNAMIC;

    case COMPOSITE_ARRAY: return csTypeArrayOf(dest, importType(dest, src, composite->inner, depth + 1));

    case COMPOSITE_UNION: {
      TypeId members[16];
      int count = composite->slotCount;
      if (count > (int)(sizeof members / sizeof members[0])) return TYPE_DYNAMIC;
      for (int i = 0; i < count; i++) members[i] = importType(dest, src, src->slots[composite->slotStart + i], depth + 1);
      return csTypeUnionOf(dest, members, count);
    }

    case COMPOSITE_FUNCTION: {
      TypeId params[32];
      int count = composite->slotCount;
      if (count > (int)(sizeof params / sizeof params[0])) return TYPE_FUNCTION;
      for (int i = 0; i < count; i++) params[i] = importType(dest, src, src->slots[composite->slotStart + i], depth + 1);
      TypeId result = importType(dest, src, composite->inner, depth + 1);
      return csTypeFunctionOf(dest, params, count, composite->requiredCount, composite->hasRest, result);
    }

    case COMPOSITE_INTERFACE: {
      /* An instantiation is rebuilt rather than copied: the generic behind it
       * is found or imported first, then instantiated here with the arguments
       * imported too. That is what makes `Map<string, number>` from this file
       * and from that one the same type. */
      if (composite->genericOf != TYPE_DYNAMIC) {
        TypeId generic = importType(dest, src, composite->genericOf, depth + 1);
        TypeId args[CS_MAX_TYPE_PARAMS];
        int argCount = composite->slotCount;
        if (argCount > CS_MAX_TYPE_PARAMS) argCount = CS_MAX_TYPE_PARAMS;
        for (int i = 0; i < argCount; i++) args[i] = importType(dest, src, src->slots[composite->slotStart + i], depth + 1);
        return csTypeInstantiate(dest, generic, args, argCount);
      }

      TypeId already = matchingInterface(dest, src, composite);
      if (already != TYPE_ERROR) return already;

      TypeId copy =
          composite->open ? csTypeDeclareClass(dest, composite->name, composite->nameLength) : csTypeDeclareInterface(dest, composite->name, composite->nameLength);
      if (copy == TYPE_ERROR) return TYPE_DYNAMIC;

      /* A generic keeps its own variables on the way across, declared here so
       * that instantiating the copy substitutes through them. */
      if (composite->typeParamCount > 0) {
        TypeId params[CS_MAX_TYPE_PARAMS];
        for (int i = 0; i < composite->typeParamCount; i++) {
          const CompositeType *variable = csTypeComposite(src, composite->typeParams[i]);
          params[i] = variable != NULL ? csTypeDeclareTypeVar(dest, variable->name, variable->nameLength) : TYPE_DYNAMIC;
          if (params[i] != TYPE_DYNAMIC) csTypeCloseTypeVar(dest, params[i]);
        }
        csTypeSetTypeParams(dest, copy, params, composite->typeParamCount);
      }

      int start = composite->memberStart;
      int count = composite->memberCount;
      for (int i = 0; i < count; i++) {
        TypeMember member = src->members[start + i];
        member.type = importType(dest, src, member.type, depth + 1);
        if (!csTypeAddMember(dest, copy, &member)) break;
      }
      return copy;
    }
  }
  return TYPE_DYNAMIC;
}

TypeId csTypeImport(TypeTable *dest, const TypeTable *src, TypeId type) {
  if (dest == NULL || src == NULL) return TYPE_DYNAMIC;
  return importType(dest, src, type, 0);
}
