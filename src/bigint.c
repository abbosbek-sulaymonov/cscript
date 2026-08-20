/* bigint.c — see bigint.h for the representation and why it is that one. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/bigint.h"

void csBigInit(BigInt *value) {
  value->limbs = NULL;
  value->count = 0;
  value->negative = false;
}

void csBigFree(BigInt *value) {
  free(value->limbs);
  csBigInit(value);
}

/* Drops leading zero limbs, and with them the possibility of a negative zero.
 * Every operation ends here, which is what makes the representation unique. */
static void trim(BigInt *value) {
  while (value->count > 0 && value->limbs[value->count - 1] == 0) value->count--;
  if (value->count == 0) value->negative = false;
}

static bool reserve(BigInt *value, int count) {
  uint32_t *limbs = (uint32_t *)calloc((size_t)(count > 0 ? count : 1), sizeof(uint32_t));
  if (limbs == NULL) return false;
  free(value->limbs);
  value->limbs = limbs;
  value->count = count;
  value->negative = false;
  return true;
}

bool csBigCopy(BigInt *out, const BigInt *from) {
  if (!reserve(out, from->count)) return false;
  memcpy(out->limbs, from->limbs, sizeof(uint32_t) * (size_t)from->count);
  out->negative = from->negative;
  return true;
}

bool csBigIsZero(const BigInt *value) { return value->count == 0; }

bool csBigFromInt64(BigInt *out, int64_t value) {
  bool negative = value < 0;
  /* Negated as unsigned, so the most negative int64 does not overflow. */
  uint64_t magnitude = negative ? ~(uint64_t)value + 1 : (uint64_t)value;

  if (!reserve(out, 2)) return false;
  out->limbs[0] = (uint32_t)(magnitude & 0xffffffffu);
  out->limbs[1] = (uint32_t)(magnitude >> 32);
  out->negative = negative;
  trim(out);
  return true;
}

/* magnitude only: |a| + |b| */
static bool addMagnitude(BigInt *out, const BigInt *a, const BigInt *b) {
  int longest = a->count > b->count ? a->count : b->count;
  if (!reserve(out, longest + 1)) return false;

  uint64_t carry = 0;
  for (int i = 0; i < longest; i++) {
    uint64_t sum = carry;
    if (i < a->count) sum += a->limbs[i];
    if (i < b->count) sum += b->limbs[i];
    out->limbs[i] = (uint32_t)(sum & 0xffffffffu);
    carry = sum >> 32;
  }
  out->limbs[longest] = (uint32_t)carry;
  trim(out);
  return true;
}

/* |a| compared with |b|, ignoring both signs. */
static int compareMagnitude(const BigInt *a, const BigInt *b) {
  if (a->count != b->count) return a->count < b->count ? -1 : 1;
  for (int i = a->count - 1; i >= 0; i--) {
    if (a->limbs[i] != b->limbs[i]) return a->limbs[i] < b->limbs[i] ? -1 : 1;
  }
  return 0;
}

/* |a| - |b|, which the caller has already checked is not negative. */
static bool subtractMagnitude(BigInt *out, const BigInt *a, const BigInt *b) {
  if (!reserve(out, a->count)) return false;

  int64_t borrow = 0;
  for (int i = 0; i < a->count; i++) {
    int64_t difference = (int64_t)a->limbs[i] - borrow - (i < b->count ? b->limbs[i] : 0);
    if (difference < 0) {
      difference += (int64_t)1 << 32;
      borrow = 1;
    } else {
      borrow = 0;
    }
    out->limbs[i] = (uint32_t)difference;
  }
  trim(out);
  return true;
}

bool csBigAdd(BigInt *out, const BigInt *a, const BigInt *b) {
  if (a->negative == b->negative) {
    if (!addMagnitude(out, a, b)) return false;
    out->negative = a->negative;
    trim(out);
    return true;
  }

  /* Different signs: the larger magnitude decides both the difference and the
   * sign of the answer. */
  int order = compareMagnitude(a, b);
  if (order == 0) return reserve(out, 0);

  const BigInt *larger = order > 0 ? a : b;
  const BigInt *smaller = order > 0 ? b : a;
  if (!subtractMagnitude(out, larger, smaller)) return false;
  out->negative = larger->negative;
  trim(out);
  return true;
}

bool csBigSubtract(BigInt *out, const BigInt *a, const BigInt *b) {
  BigInt flipped;
  csBigInit(&flipped);
  if (!csBigCopy(&flipped, b)) return false;
  if (!csBigIsZero(&flipped)) flipped.negative = !flipped.negative;

  bool ok = csBigAdd(out, a, &flipped);
  csBigFree(&flipped);
  return ok;
}

bool csBigMultiply(BigInt *out, const BigInt *a, const BigInt *b) {
  if (csBigIsZero(a) || csBigIsZero(b)) return reserve(out, 0);

  BigInt product;
  csBigInit(&product);
  if (!reserve(&product, a->count + b->count)) return false;

  for (int i = 0; i < a->count; i++) {
    uint64_t carry = 0;
    for (int j = 0; j < b->count; j++) {
      uint64_t at = (uint64_t)product.limbs[i + j] +
                    (uint64_t)a->limbs[i] * (uint64_t)b->limbs[j] + carry;
      product.limbs[i + j] = (uint32_t)(at & 0xffffffffu);
      carry = at >> 32;
    }
    product.limbs[i + b->count] += (uint32_t)carry;
  }

  product.negative = a->negative != b->negative;
  trim(&product);

  csBigFree(out);
  *out = product;
  return true;
}

/* |a| divided by one limb, in place, answering the remainder. */
static uint32_t divideBySmall(BigInt *value, uint32_t divisor) {
  uint64_t remainder = 0;
  for (int i = value->count - 1; i >= 0; i--) {
    uint64_t current = (remainder << 32) | value->limbs[i];
    value->limbs[i] = (uint32_t)(current / divisor);
    remainder = current % divisor;
  }
  trim(value);
  return (uint32_t)remainder;
}

/* Schoolbook long division, one bit at a time.
 *
 * Knuth's algorithm D is much faster and much easier to get subtly wrong. The
 * numbers a script divides are small, and being able to read this and believe
 * it is worth more here than the constant factor. */
static bool divideMagnitude(BigInt *quotient, BigInt *remainder, const BigInt *a,
                            const BigInt *b) {
  if (!reserve(quotient, a->count)) return false;
  if (!reserve(remainder, 0)) return false;

  for (int i = a->count * 32 - 1; i >= 0; i--) {
    /* remainder = remainder * 2 + bit i of a */
    BigInt doubled;
    csBigInit(&doubled);
    if (!csBigAdd(&doubled, remainder, remainder)) return false;

    uint32_t bit = (a->limbs[i / 32] >> (i % 32)) & 1u;
    if (bit != 0) {
      BigInt one;
      csBigInit(&one);
      if (!csBigFromInt64(&one, 1)) return false;
      BigInt raised;
      csBigInit(&raised);
      if (!csBigAdd(&raised, &doubled, &one)) return false;
      csBigFree(&doubled);
      csBigFree(&one);
      doubled = raised;
    }

    csBigFree(remainder);
    *remainder = doubled;

    if (compareMagnitude(remainder, b) >= 0) {
      BigInt reduced;
      csBigInit(&reduced);
      if (!subtractMagnitude(&reduced, remainder, b)) return false;
      csBigFree(remainder);
      *remainder = reduced;
      quotient->limbs[i / 32] |= 1u << (i % 32);
    }
  }

  trim(quotient);
  trim(remainder);
  return true;
}

bool csBigDivide(BigInt *out, const BigInt *a, const BigInt *b) {
  if (csBigIsZero(b)) return false;

  BigInt magnitudeA;
  BigInt magnitudeB;
  csBigInit(&magnitudeA);
  csBigInit(&magnitudeB);
  if (!csBigCopy(&magnitudeA, a) || !csBigCopy(&magnitudeB, b)) return false;
  magnitudeA.negative = false;
  magnitudeB.negative = false;

  BigInt quotient;
  BigInt remainder;
  csBigInit(&quotient);
  csBigInit(&remainder);
  bool ok = divideMagnitude(&quotient, &remainder, &magnitudeA, &magnitudeB);

  csBigFree(&magnitudeA);
  csBigFree(&magnitudeB);
  csBigFree(&remainder);
  if (!ok) {
    csBigFree(&quotient);
    return false;
  }

  quotient.negative = a->negative != b->negative;
  trim(&quotient);
  csBigFree(out);
  *out = quotient;
  return true;
}

bool csBigRemainder(BigInt *out, const BigInt *a, const BigInt *b) {
  if (csBigIsZero(b)) return false;

  BigInt magnitudeA;
  BigInt magnitudeB;
  csBigInit(&magnitudeA);
  csBigInit(&magnitudeB);
  if (!csBigCopy(&magnitudeA, a) || !csBigCopy(&magnitudeB, b)) return false;
  magnitudeA.negative = false;
  magnitudeB.negative = false;

  BigInt quotient;
  BigInt remainder;
  csBigInit(&quotient);
  csBigInit(&remainder);
  bool ok = divideMagnitude(&quotient, &remainder, &magnitudeA, &magnitudeB);

  csBigFree(&magnitudeA);
  csBigFree(&magnitudeB);
  csBigFree(&quotient);
  if (!ok) {
    csBigFree(&remainder);
    return false;
  }

  /* The remainder takes the sign of the dividend, which is what makes
   * `-7n % 3n` equal `-1n` rather than `2n`. */
  remainder.negative = a->negative;
  trim(&remainder);
  csBigFree(out);
  *out = remainder;
  return true;
}

bool csBigPower(BigInt *out, const BigInt *base, const BigInt *exponent) {
  if (exponent->negative) return false;

  /* Square and multiply, over the exponent's bits from the top. */
  BigInt result;
  csBigInit(&result);
  if (!csBigFromInt64(&result, 1)) return false;

  for (int i = exponent->count * 32 - 1; i >= 0; i--) {
    BigInt squared;
    csBigInit(&squared);
    if (!csBigMultiply(&squared, &result, &result)) return false;
    csBigFree(&result);
    result = squared;

    if (((exponent->limbs[i / 32] >> (i % 32)) & 1u) != 0) {
      BigInt scaled;
      csBigInit(&scaled);
      if (!csBigMultiply(&scaled, &result, base)) return false;
      csBigFree(&result);
      result = scaled;
    }
  }

  csBigFree(out);
  *out = result;
  return true;
}

bool csBigNegate(BigInt *out, const BigInt *value) {
  if (!csBigCopy(out, value)) return false;
  if (!csBigIsZero(out)) out->negative = !out->negative;
  return true;
}

int csBigCompare(const BigInt *a, const BigInt *b) {
  if (a->negative != b->negative) return a->negative ? -1 : 1;
  int order = compareMagnitude(a, b);
  return a->negative ? -order : order;
}

static int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

bool csBigFromText(BigInt *out, const char *text, int length) {
  int at = 0;
  bool negative = false;
  if (at < length && (text[at] == '+' || text[at] == '-')) {
    negative = text[at] == '-';
    at++;
  }

  int radix = 10;
  if (at + 1 < length && text[at] == '0') {
    char marker = text[at + 1];
    if (marker == 'x' || marker == 'X') { radix = 16; at += 2; }
    else if (marker == 'o' || marker == 'O') { radix = 8; at += 2; }
    else if (marker == 'b' || marker == 'B') { radix = 2; at += 2; }
  }
  if (at >= length) return false;

  if (!reserve(out, 0)) return false;

  BigInt scale;
  BigInt digit;
  csBigInit(&scale);
  csBigInit(&digit);
  if (!csBigFromInt64(&scale, radix)) return false;

  for (; at < length; at++) {
    if (text[at] == '_') continue; /* the same separator a number may have */
    int value = digitValue(text[at]);
    if (value < 0 || value >= radix) {
      csBigFree(&scale);
      csBigFree(&digit);
      return false;
    }

    BigInt shifted;
    csBigInit(&shifted);
    if (!csBigMultiply(&shifted, out, &scale)) return false;
    if (!csBigFromInt64(&digit, value)) return false;

    BigInt summed;
    csBigInit(&summed);
    if (!csBigAdd(&summed, &shifted, &digit)) return false;

    csBigFree(&shifted);
    csBigFree(out);
    *out = summed;
  }

  csBigFree(&scale);
  csBigFree(&digit);
  out->negative = negative;
  trim(out);
  return true;
}

bool csBigFromDouble(BigInt *out, double value) {
  if (isnan(value) || isinf(value)) return false;
  if (value != trunc(value)) return false; /* a fraction is not a whole number */

  /* Every finite double is exactly mantissa x 2^exponent with a mantissa of at
   * most 53 bits, so scaling the mantissa to a whole number and then applying
   * the exponent is exact for the whole range — including the values far above
   * what an int64 holds, which is the range a BigInt is asked for. */
  int exponent = 0;
  double mantissa = ldexp(frexp(value, &exponent), 53);
  exponent -= 53;

  if (!csBigFromInt64(out, (int64_t)mantissa)) return false;
  if (exponent == 0) return true;

  BigInt two;
  BigInt scale;
  BigInt power;
  csBigInit(&two);
  csBigInit(&scale);
  csBigInit(&power);
  bool ok = csBigFromInt64(&two, 2) &&
            csBigFromInt64(&power, exponent > 0 ? exponent : -exponent) &&
            csBigPower(&scale, &two, &power);
  if (ok) {
    BigInt scaled;
    csBigInit(&scaled);
    /* Dividing is exact here: `value` is whole, so the low bits it is being
     * shifted down past are all zero. */
    ok = exponent > 0 ? csBigMultiply(&scaled, out, &scale)
                      : csBigDivide(&scaled, out, &scale);
    if (ok) {
      csBigFree(out);
      *out = scaled;
    } else {
      csBigFree(&scaled);
    }
  }

  csBigFree(&two);
  csBigFree(&scale);
  csBigFree(&power);
  return ok;
}

/* Where `value` sits relative to `number`, exactly — without rounding the
 * BigInt to a double, which is the whole point of having one. NaN has no
 * ordering at all, which the caller has to handle, so it is reported as 2. */
int csBigCompareDouble(const BigInt *value, double number) {
  if (isnan(number)) return 2;
  if (isinf(number)) return number > 0 ? -1 : 1;

  BigInt whole;
  csBigInit(&whole);
  if (!csBigFromDouble(&whole, floor(number))) {
    csBigFree(&whole);
    return 2;
  }

  int order = csBigCompare(value, &whole);
  csBigFree(&whole);

  /* Equal to the floor, but the number had something after the point: then the
   * number is the larger of the two. */
  if (order == 0 && number != floor(number)) return -1;
  return order;
}

double csBigToDouble(const BigInt *value) {
  double result = 0;
  for (int i = value->count - 1; i >= 0; i--) {
    result = result * 4294967296.0 + (double)value->limbs[i];
  }
  return value->negative ? -result : result;
}

char *csBigToText(const BigInt *value, int radix) {
  if (csBigIsZero(value)) {
    char *zero = (char *)malloc(2);
    if (zero == NULL) return NULL;
    zero[0] = '0';
    zero[1] = '\0';
    return zero;
  }

  BigInt working;
  csBigInit(&working);
  if (!csBigCopy(&working, value)) return NULL;
  working.negative = false;

  /* Ten bits a limb is more than any radix needs, plus the sign and the NUL. */
  size_t capacity = (size_t)working.count * 34 + 2;
  char *digits = (char *)malloc(capacity);
  if (digits == NULL) {
    csBigFree(&working);
    return NULL;
  }

  size_t at = capacity;
  digits[--at] = '\0';
  while (!csBigIsZero(&working)) {
    uint32_t remainder = divideBySmall(&working, (uint32_t)radix);
    digits[--at] = (char)(remainder < 10 ? '0' + remainder : 'a' + remainder - 10);
  }
  if (value->negative) digits[--at] = '-';

  memmove(digits, digits + at, capacity - at);
  csBigFree(&working);
  return digits;
}
