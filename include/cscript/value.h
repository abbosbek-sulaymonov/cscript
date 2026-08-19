/* value.h — the tagged union every CScript value is carried in.
 *
 * A Value is 16 bytes: a type tag plus the payload. NaN-boxing would fold this
 * to 8 and is a planned optimisation, which is why nothing outside this header
 * touches the representation directly — always go through the macros.
 */
#ifndef CSCRIPT_VALUE_H
#define CSCRIPT_VALUE_H

#include "cscript/common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;

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

/* Constructors. */
#define UNDEFINED_VAL   ((Value){VAL_UNDEFINED, {.number = 0}})
#define NULL_VAL        ((Value){VAL_NULL, {.number = 0}})
#define BOOL_VAL(b)     ((Value){VAL_BOOL, {.boolean = (b)}})
#define NUMBER_VAL(n)   ((Value){VAL_NUMBER, {.number = (n)}})
#define OBJ_VAL(o)      ((Value){VAL_OBJ, {.obj = (Obj *)(o)}})

/* Type tests. */
#define IS_UNDEFINED(v) ((v).type == VAL_UNDEFINED)
#define IS_NULL(v)      ((v).type == VAL_NULL)
#define IS_BOOL(v)      ((v).type == VAL_BOOL)
#define IS_NUMBER(v)    ((v).type == VAL_NUMBER)
#define IS_OBJ(v)       ((v).type == VAL_OBJ)

/* Unwrapping. Only valid after the matching IS_ check. */
#define AS_BOOL(v)      ((v).as.boolean)
#define AS_NUMBER(v)    ((v).as.number)
#define AS_OBJ(v)       ((v).as.obj)

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

/* Writes the value to stdout the way `print` renders it. */
void csValuePrint(Value value);

/* Renders into a freshly allocated C string the caller must free(). */
char *csValueToCString(Value value, size_t *lengthOut);

#endif /* CSCRIPT_VALUE_H */
