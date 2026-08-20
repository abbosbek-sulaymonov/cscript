/* bigint.h — arbitrary-precision integers, sign and magnitude.
 *
 * The magnitude is an array of 32-bit limbs, least significant first, with no
 * leading zero limb — so the representation of a value is unique and equality
 * is a length check and a memcmp. Thirty-two bits rather than sixty-four
 * because every intermediate product then fits in a `uint64_t`, which is what
 * keeps the multiply and the divide readable and portable.
 *
 * Nothing here allocates through the collector: a BigInt's limbs are plain
 * malloc, owned by the object that holds them and freed with it.
 */
#ifndef CSCRIPT_BIGINT_H
#define CSCRIPT_BIGINT_H

#include "cscript/common.h"

typedef struct {
  uint32_t *limbs; /* least significant first, no leading zero */
  int count;
  bool negative; /* zero is never negative */
} BigInt;

/* Every one of these leaves `out` owning freshly allocated limbs, which the
 * caller frees with csBigFree. They return false only when they could not
 * allocate, except where noted. */
void csBigInit(BigInt *value);
void csBigFree(BigInt *value);
bool csBigCopy(BigInt *out, const BigInt *from);

bool csBigFromInt64(BigInt *out, int64_t value);
/* Reads decimal, or `0x`/`0o`/`0b` with the matching digits. False when the
 * text is not a whole number in that form. */
bool csBigFromText(BigInt *out, const char *text, int length);
/* Whole doubles only: a fraction has no BigInt. */
bool csBigFromDouble(BigInt *out, double value);

/* Base 2 to 36. The caller frees the result. */
char *csBigToText(const BigInt *value, int radix);
double csBigToDouble(const BigInt *value);

int csBigCompare(const BigInt *a, const BigInt *b);
/* Exact, without rounding either side. Answers 2 when there is no ordering,
 * which happens only for NaN. */
int csBigCompareDouble(const BigInt *value, double number);
bool csBigIsZero(const BigInt *value);

bool csBigAdd(BigInt *out, const BigInt *a, const BigInt *b);
bool csBigSubtract(BigInt *out, const BigInt *a, const BigInt *b);
bool csBigMultiply(BigInt *out, const BigInt *a, const BigInt *b);
/* Truncating, as JavaScript's `/` on BigInt is. False when `b` is zero. */
bool csBigDivide(BigInt *out, const BigInt *a, const BigInt *b);
/* The remainder takes the sign of `a`, as `%` does. False when `b` is zero. */
bool csBigRemainder(BigInt *out, const BigInt *a, const BigInt *b);
/* False for a negative exponent, which has no whole answer. */
bool csBigPower(BigInt *out, const BigInt *base, const BigInt *exponent);
bool csBigNegate(BigInt *out, const BigInt *value);

#endif /* CSCRIPT_BIGINT_H */
