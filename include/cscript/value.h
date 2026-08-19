/* value.h — the runtime representation of a CScript value.
 *
 * Two representations live behind one interface, chosen at build time.
 *
 *   Tagged union (16 bytes): a type tag plus a payload. Obvious to read and to
 *   debug, and portable to anything.
 *
 *   NaN-boxed (8 bytes): everything packed into a double. IEEE 754 leaves a
 *   huge space of bit patterns that all mean "not a number" — 2^51 of them —
 *   and nothing but a signalling NaN needs more than one. Every non-number
 *   value hides in there, so a Value fits in a register and the value stack
 *   moves half as many bytes.
 *
 * Nothing outside this header touches the representation: everything goes
 * through the IS_ / AS_ / _VAL macros, which is what made adding the second
 * representation a change to one file rather than to the whole interpreter.
 *
 * Build with CS_NAN_BOXING=0 to force the tagged union; `make test-tagged`
 * runs the suite that way so both stay correct.
 */
#ifndef CSCRIPT_VALUE_H
#define CSCRIPT_VALUE_H

#include <string.h>

#include "cscript/common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct Shape Shape;

/* NaN-boxing assumes 64-bit doubles and pointers that fit in 48 bits, which is
 * true of x86-64 and arm64 but not of every target. */
#ifndef CS_NAN_BOXING
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(__arm64__)
#define CS_NAN_BOXING 1
#else
#define CS_NAN_BOXING 0
#endif
#endif

#if CS_NAN_BOXING

typedef uint64_t Value;

/* A quiet NaN with the "not a signalling NaN" bit set. Any value carrying this
 * pattern is one of ours rather than a real number. */
#define CS_QNAN ((uint64_t)0x7ffc000000000000)
#define CS_SIGN_BIT ((uint64_t)0x8000000000000000)

/* The sign bit distinguishes objects from singletons, leaving 48 bits of
 * pointer — enough for every address a userspace program sees. */
/* The two boolean tags differ only in the low bit, which is what lets IS_BOOL
 * test both with a single OR. The other two singletons must therefore avoid
 * the pair {4, 5}. */
#define CS_TAG_UNDEFINED 1
#define CS_TAG_NULL 2
#define CS_TAG_FALSE 4
#define CS_TAG_TRUE 5

#define UNDEFINED_VAL ((Value)(CS_QNAN | CS_TAG_UNDEFINED))
#define NULL_VAL ((Value)(CS_QNAN | CS_TAG_NULL))
#define FALSE_VAL ((Value)(CS_QNAN | CS_TAG_FALSE))
#define TRUE_VAL ((Value)(CS_QNAN | CS_TAG_TRUE))
#define BOOL_VAL(b) ((b) ? TRUE_VAL : FALSE_VAL)
#define NUMBER_VAL(n) csNumberToValue(n)
#define OBJ_VAL(o) ((Value)(CS_SIGN_BIT | CS_QNAN | (uint64_t)(uintptr_t)(Obj *)(o)))

#define IS_UNDEFINED(v) ((v) == UNDEFINED_VAL)
#define IS_NULL(v) ((v) == NULL_VAL)
#define IS_BOOL(v) (((v) | 1) == TRUE_VAL)
/* A real number is anything that is *not* wearing the quiet-NaN pattern. */
#define IS_NUMBER(v) (((v) & CS_QNAN) != CS_QNAN)
#define IS_OBJ(v) (((v) & (CS_QNAN | CS_SIGN_BIT)) == (CS_QNAN | CS_SIGN_BIT))

#define AS_BOOL(v) ((v) == TRUE_VAL)
#define AS_NUMBER(v) csValueToDouble(v)
#define AS_OBJ(v) ((Obj *)(uintptr_t)((v) & ~(CS_SIGN_BIT | CS_QNAN)))

/* memcpy is the only strictly-conforming way to reinterpret the bits, and every
 * compiler folds it away at -O2. */
static inline Value csNumberToValue(double number) {
  Value value;
  memcpy(&value, &number, sizeof(double));
  return value;
}

static inline double csValueToDouble(Value value) {
  double number;
  memcpy(&number, &value, sizeof(Value));
  return number;
}

#else /* tagged union */

typedef enum {
  VAL_UNDEFINED,
  VAL_NULL,
  VAL_BOOL,
  VAL_NUMBER,
  VAL_OBJ,
} ValueType;

typedef struct {
  ValueType type;
  union {
    bool boolean;
    double number;
    Obj *obj;
  } as;
} Value;

#define UNDEFINED_VAL ((Value){VAL_UNDEFINED, {.number = 0}})
#define NULL_VAL ((Value){VAL_NULL, {.number = 0}})
#define BOOL_VAL(b) ((Value){VAL_BOOL, {.boolean = (b)}})
#define NUMBER_VAL(n) ((Value){VAL_NUMBER, {.number = (n)}})
#define OBJ_VAL(o) ((Value){VAL_OBJ, {.obj = (Obj *)(o)}})

#define IS_UNDEFINED(v) ((v).type == VAL_UNDEFINED)
#define IS_NULL(v) ((v).type == VAL_NULL)
#define IS_BOOL(v) ((v).type == VAL_BOOL)
#define IS_NUMBER(v) ((v).type == VAL_NUMBER)
#define IS_OBJ(v) ((v).type == VAL_OBJ)

#define AS_BOOL(v) ((v).as.boolean)
#define AS_NUMBER(v) ((v).as.number)
#define AS_OBJ(v) ((v).as.obj)

#endif /* CS_NAN_BOXING */

/* A growable Value array, used for a chunk's constant pool. */
typedef struct {
  int count;
  int capacity;
  Value *values;
} ValueArray;

void csValueArrayInit(ValueArray *array);
void csValueArrayWrite(ValueArray *array, Value value);
void csValueArrayFree(ValueArray *array);

/* `===`: no coercion, different types are never equal. CScript has no coercing
 * equality at all — see docs/GRAMMAR.md for why. */
bool csValuesStrictEqual(Value a, Value b);

/* Explicit ToNumber, exposed for the Number() built-in. Never applied
 * implicitly by any operator. */
double csValueToNumber(Value value);

/* JS truthiness: false, null, undefined, 0, NaN and "" are falsy. */
bool csValueIsTruthy(Value value);

/* The string `typeof` would produce. Static storage, never freed. */
const char *csValueTypeName(Value value);

/* Writes the value to stdout the way `console.log` renders it. */
void csValuePrint(Value value);

/* String conversion, as `String(x)` and `"" + x` produce it. Caller free()s. */
char *csValueToCString(Value value, size_t *lengthOut);

/* Display form, as console.log prints it. Differs from the above in two ways
 * that JavaScript also distinguishes between String() and util.inspect():
 * -0 keeps its sign, and strings inside a container are quoted. */
char *csValueInspect(Value value, size_t *lengthOut);

#endif /* CSCRIPT_VALUE_H */
