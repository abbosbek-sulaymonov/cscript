/* type.h — the static type lattice.
 *
 * Types are deliberately shallow: a fixed set of primitive kinds, no structural
 * or higher-order types. That is what keeps gradual typing cheap here. Sound
 * gradual type systems get expensive at the boundary between typed and untyped
 * code, because a function or object crossing it has to be wrapped in a
 * contract that checks every later use. A primitive needs one check, or none.
 *
 * Annotations are optional. An unannotated declaration takes the type of its
 * initialiser, so most code is checked without being annotated at all.
 */
#ifndef CSCRIPT_TYPE_H
#define CSCRIPT_TYPE_H

#include "cscript/common.h"

typedef enum {
  /* The dynamic type. Assignable to and from everything, which is what makes
   * the system gradual rather than static. */
  TYPE_ANY,

  TYPE_NUMBER,
  TYPE_STRING,
  TYPE_BOOLEAN,
  TYPE_NULL,
  TYPE_UNDEFINED,
  TYPE_FUNCTION,
  TYPE_OBJECT,

  /* An error was already reported for this expression. It absorbs every
   * operation silently, so one bad subexpression does not produce a cascade of
   * complaints about everything built on top of it. */
  TYPE_ERROR,
} TypeKind;

/* The name used in messages and accepted in annotations. */
const char *csTypeName(TypeKind type);

/* Parses an annotation like `number`. Returns false when the name is unknown. */
bool csTypeFromName(const char *name, int length, TypeKind *out);

/* Can a value of `from` be stored where `to` is expected?
 *
 * `any` is assignable in both directions — that is the gradual escape hatch and
 * also the one place the checker deliberately stops being sound. Everything
 * else requires an exact match. */
bool csTypeAssignable(TypeKind from, TypeKind to);

/* True when the type is known well enough to specialise code for it. */
static inline bool csTypeIsKnown(TypeKind type) {
  return type != TYPE_ANY && type != TYPE_ERROR;
}

#endif /* CSCRIPT_TYPE_H */
