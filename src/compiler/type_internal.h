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

#endif /* CSCRIPT_COMPILER_TYPE_INTERNAL_H */
