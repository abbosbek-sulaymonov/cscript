/* type_internal.h — what the two halves of the type system share.
 *
 * type.c holds the names and the relation over them; type_table.c holds the
 * table the composite types live in. The one thing both need is the name
 * comparison below, and nothing outside them includes this.
 */
#ifndef CSCRIPT_COMPILER_TYPE_INTERNAL_H
#define CSCRIPT_COMPILER_TYPE_INTERNAL_H

#include "cscript/type.h"

/* A NUL-terminated candidate against a name that may not be. */
bool csTypeNameMatches(const char *name, int length, const char *candidate);

/* A blank composite of the given kind, or NULL when the table is full. Shared
 * because every kind of type is built the same way. */
CompositeType *csTypeNewComposite(TypeTable *table, CompositeKind kind, TypeId *idOut);

/* Finishes an unevaluated `keyof`, indexed access or mapped type once its
 * pieces are concrete. Answers the type unchanged when they are not. */
TypeId csTypeEvaluate(TypeTable *table, TypeId type);

/* Applies a utility type — `Partial<T>`, `Pick<T, K>` and the rest — by name.
 * TYPE_ERROR when the name is not one of them, and `wanted` says how many
 * arguments it takes so a miscount reads like any other generic's. */
TypeId csTypeUtility(TypeTable *table, const char *name, int length, const TypeId *args, int argCount, int *wanted);

/* Reserves a run of the slot pool and fills it. False when the table is full,
 * which is the one failure every constructor here degrades to DYNAMIC on. */
bool csTypeTakeSlots(TypeTable *table, const TypeId *values, int count, int *startOut);

#endif /* CSCRIPT_COMPILER_TYPE_INTERNAL_H */
