/* value_internal.h — the seam between value.c and value_render.c.
 *
 * Nothing outside src/runtime/value*.c includes this.
 */
#ifndef CSCRIPT_RUNTIME_VALUE_INTERNAL_H
#define CSCRIPT_RUNTIME_VALUE_INTERNAL_H

#include "cscript/value.h"

/* A double, formatted the way ECMA-262's `Number::toString` says — which is
 * not printf's `%g`, and is the reason `0.1 + 0.2` and `1e21` print the way
 * JavaScript prints them. Returns the length written.
 *
 * `signedZero` controls whether -0 renders as "-0" or as "0". String
 * conversion gives "0", because that is what `String(-0)` returns;
 * `console.log` shows the sign, because otherwise a negative zero is
 * invisible. JavaScript draws the same distinction between `String()` and
 * `util.inspect()`. */
int csValueFormatNumberEx(char *buffer, size_t size, double value, bool signedZero);
int csValueFormatNumber(char *buffer, size_t size, double value);

#endif /* CSCRIPT_RUNTIME_VALUE_INTERNAL_H */
