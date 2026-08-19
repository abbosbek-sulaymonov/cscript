#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/table.h"
#include "cscript/value.h"

void csValueArrayInit(ValueArray *array) {
  array->count = 0;
  array->capacity = 0;
  array->values = NULL;
}

void csValueArrayWrite(ValueArray *array, Value value) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = CS_GROW_CAPACITY(oldCapacity);
    array->values = CS_GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
  }
  array->values[array->count++] = value;
}

void csValueArrayFree(ValueArray *array) {
  CS_FREE_ARRAY(Value, array->values, array->capacity);
  csValueArrayInit(array);
}

bool csValuesStrictEqual(Value a, Value b) {
  /* Numbers are compared as doubles rather than as bit patterns, which is what
   * makes NaN !== NaN and -0 === 0 come out right. Under NaN-boxing a raw
   * comparison would get both of those backwards. */
  if (IS_NUMBER(a) && IS_NUMBER(b)) return AS_NUMBER(a) == AS_NUMBER(b);
  if (IS_NUMBER(a) != IS_NUMBER(b)) return false;

  if (IS_OBJ(a) && IS_OBJ(b)) {
    /* Strings are interned, so pointer identity is value equality. */
    return AS_OBJ(a) == AS_OBJ(b);
  }
  if (IS_OBJ(a) != IS_OBJ(b)) return false;

  if (IS_BOOL(a) && IS_BOOL(b)) return AS_BOOL(a) == AS_BOOL(b);
  if (IS_NULL(a) && IS_NULL(b)) return true;
  if (IS_UNDEFINED(a) && IS_UNDEFINED(b)) return true;
  return false;
}

/* Coerces a value to a number the way JS's abstract ToNumber does. This is only
 * ever reached through the Number() built-in — no operator applies it. */
double csValueToNumber(Value value) {
  if (IS_NUMBER(value)) return AS_NUMBER(value);
  if (IS_UNDEFINED(value)) return NAN;
  if (IS_NULL(value)) return 0;
  if (IS_BOOL(value)) return AS_BOOL(value) ? 1 : 0;

  if (IS_STRING(value)) {
    ObjString *string = AS_STRING(value);
    if (string->length == 0) return 0; /* Number("") is 0 */
    char *end = NULL;
    double result = strtod(string->chars, &end);
    /* Trailing non-space means the whole string was not a number. */
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (end != string->chars + string->length) return NAN;
    return result;
  }
  return NAN;
}

bool csValueIsTruthy(Value value) {
  if (IS_NUMBER(value)) {
    double n = AS_NUMBER(value);
    return n != 0 && !isnan(n); /* 0, -0 and NaN are falsy */
  }
  if (IS_BOOL(value)) return AS_BOOL(value);
  if (IS_NULL(value) || IS_UNDEFINED(value)) return false;
  if (IS_STRING(value)) return AS_STRING(value)->length > 0;
  return true; /* every other object is truthy */
}

const char *csValueTypeName(Value value) {
  if (IS_NUMBER(value)) return "number";
  if (IS_BOOL(value)) return "boolean";
  if (IS_UNDEFINED(value)) return "undefined";
  /* JavaScript reports "object" here. That is a bug from 1995 that cannot be
   * fixed without breaking the web; CScript is not bound by that. */
  if (IS_NULL(value)) return "null";
  if (IS_STRING(value)) return "string";
  /* Natives, user functions and closures are all callable, so `typeof` cannot
   * tell them apart — which matches JavaScript. */
  if (IS_NATIVE(value) || IS_FUNCTION(value) || IS_CLOSURE(value) ||
      IS_BOUND_METHOD(value)) {
    return "function";
  }
  /* A class is callable in JavaScript — only with `new` — and reports as a
   * function. CScript keeps the report and rejects the call. */
  if (IS_CLASS(value)) return "function";
  return "object";
}

/* printf writes an exponent with at least two digits — "1e-07" — while
 * JavaScript writes the minimum — "1e-7". Rewrites the buffer in place. */
/* Number-to-string, following ECMA-262's Number::toString rather than C's %g.
 *
 * The two disagree about when to use exponent notation. C switches once the
 * exponent leaves [-4, precision); JavaScript switches once the decimal point
 * would fall outside (-6, 21]. So 1e20 prints in full and 1e21 does not, and
 * 1e-6 prints in full while 1e-7 does not. printf also pads the exponent to two
 * digits, writing "1e-07" where JavaScript writes "1e-7".
 *
 * The shape is: take the shortest decimal digit string that reads back as the
 * same double, then decide where the decimal point goes.
 */

/* Fills `digits` with the shortest significant digits that round-trip and sets
 * `pointPosition` to where the decimal point falls within them — so the value
 * is 0.<digits> x 10^pointPosition. Returns the digit count. */
static int shortestDigits(double value, char *digits, size_t size,
                          int *pointPosition) {
  char scratch[64];

  for (int precision = 0; precision < 17; precision++) {
    snprintf(scratch, sizeof(scratch), "%.*e", precision, value);
    if (strtod(scratch, NULL) != value) continue;

    /* scratch looks like "d.ddde+XX" or "de+XX". */
    char *marker = strchr(scratch, 'e');
    if (marker == NULL) break;
    int exponent = (int)strtol(marker + 1, NULL, 10);
    *marker = '\0';

    int count = 0;
    for (const char *c = scratch; *c != '\0'; c++) {
      if (*c >= '0' && *c <= '9' && (size_t)count < size - 1) digits[count++] = *c;
    }

    /* Once the value round-trips, trailing zeros carry no information. */
    while (count > 1 && digits[count - 1] == '0') count--;
    digits[count] = '\0';

    *pointPosition = exponent + 1;
    return count;
  }

  digits[0] = '0';
  digits[1] = '\0';
  *pointPosition = 1;
  return 1;
}

/* `signedZero` controls whether -0 renders as "-0" or as "0". String conversion
 * gives "0", because that is what String(-0) returns; console.log shows the
 * sign, because otherwise a negative zero is invisible. JavaScript draws the
 * same distinction between String() and util.inspect(). */
static int formatNumberEx(char *buffer, size_t size, double value, bool signedZero) {
  if (isnan(value)) return snprintf(buffer, size, "NaN");
  if (isinf(value)) return snprintf(buffer, size, value > 0 ? "Infinity" : "-Infinity");
  if (value == 0) {
    return snprintf(buffer, size, signedZero && signbit(value) ? "-0" : "0");
  }

  const char *sign = "";
  if (value < 0) {
    sign = "-";
    value = -value;
  }

  char digits[32];
  int point = 0;
  int count = shortestDigits(value, digits, sizeof(digits), &point);

  /* All the digits, then zeros out to the decimal point: 1e20. */
  if (count <= point && point <= 21) {
    int written = snprintf(buffer, size, "%s%s", sign, digits);
    while (written < (int)size - 1 && written - (int)strlen(sign) < point) {
      buffer[written++] = '0';
    }
    buffer[written] = '\0';
    return written;
  }

  /* The point falls inside the digits: 1.5, 123.456. */
  if (0 < point && point <= 21) {
    return snprintf(buffer, size, "%s%.*s.%s", sign, point, digits, digits + point);
  }

  /* Leading zeros, down to 1e-6: 0.000001. */
  if (-6 < point && point <= 0) {
    int written = snprintf(buffer, size, "%s0.", sign);
    for (int i = 0; i < -point && written < (int)size - 1; i++) buffer[written++] = '0';
    buffer[written] = '\0';
    return written + snprintf(buffer + written, size - (size_t)written, "%s", digits);
  }

  /* Everything else is exponential, with the exponent written in the fewest
   * digits and always carrying a sign. */
  int exponent = point - 1;
  if (count == 1) {
    return snprintf(buffer, size, "%s%se%+d", sign, digits, exponent);
  }
  return snprintf(buffer, size, "%s%.1s.%se%+d", sign, digits, digits + 1, exponent);
}

static int formatNumber(char *buffer, size_t size, double value) {
  return formatNumberEx(buffer, size, value, false);
}

void csValuePrint(Value value) {
  if (IS_NUMBER(value)) {
    char buffer[32];
    formatNumberEx(buffer, sizeof(buffer), AS_NUMBER(value), true);
    printf("%s", buffer);
  } else if (IS_BOOL(value)) {
    printf(AS_BOOL(value) ? "true" : "false");
  } else if (IS_NULL(value)) {
    printf("null");
  } else if (IS_UNDEFINED(value)) {
    printf("undefined");
  } else {
    csObjectPrint(value);
  }
}

/* A small growable buffer, used to render containers. Kept local to this file
 * because nothing else needs it yet. */
typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} StringBuilder;

static bool sbAppend(StringBuilder *builder, const char *text, size_t length) {
  if (builder->length + length + 1 > builder->capacity) {
    size_t capacity = builder->capacity < 32 ? 32 : builder->capacity;
    while (capacity < builder->length + length + 1) capacity *= 2;
    char *grown = (char *)realloc(builder->data, capacity);
    if (grown == NULL) return false;
    builder->data = grown;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, text, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool sbAppendValue(StringBuilder *builder, Value value, bool quoteStrings);

/* `Promise { 1 }`, `Promise { <pending> }`, `Promise { <rejected> 'why' }` —
 * the same shapes Node prints, so a program that logs one still matches. */
static bool sbAppendPromise(StringBuilder *builder, ObjPromise *promise) {
  if (promise->state == PROMISE_PENDING) {
    return sbAppend(builder, "Promise { <pending> }", 21);
  }
  if (!sbAppend(builder, "Promise { ", 10)) return false;
  if (promise->state == PROMISE_REJECTED &&
      !sbAppend(builder, "<rejected> ", 11)) {
    return false;
  }
  return sbAppendValue(builder, promise->value, true) && sbAppend(builder, " }", 2);
}

static bool sbAppendArray(StringBuilder *builder, ObjArray *array) {
  if (!sbAppend(builder, "[ ", array->elements.count > 0 ? 2 : 1)) return false;
  for (int i = 0; i < array->elements.count; i++) {
    if (i > 0 && !sbAppend(builder, ", ", 2)) return false;
    /* Strings are quoted inside a container so `[ '1' ]` and `[ 1 ]` differ. */
    if (!sbAppendValue(builder, array->elements.values[i], true)) return false;
  }
  return sbAppend(builder, array->elements.count > 0 ? " ]" : "]",
                  array->elements.count > 0 ? 2 : 1);
}

static bool sbAppendObject(StringBuilder *builder, ObjObject *object) {
  /* An instance prints under its class name — `Dog { name: 'Rex' }` — which is
   * what makes one distinguishable from a plain literal at a glance. */
  if (object->klass != NULL) {
    if (!sbAppend(builder, object->klass->name->chars,
                  (size_t)object->klass->name->length)) {
      return false;
    }
    if (!sbAppend(builder, " ", 1)) return false;
  }
  if (!sbAppend(builder, "{", 1)) return false;

  bool first = true;
  for (int i = 0; i < csObjectCount(object); i++) {
    ObjString *key = csObjectKeyAt(object, i);
    Value value = csObjectValueAt(object, i);

    if (!sbAppend(builder, first ? " " : ", ", first ? 1 : 2)) return false;
    first = false;
    if (!sbAppend(builder, key->chars, (size_t)key->length)) return false;
    if (!sbAppend(builder, ": ", 2)) return false;
    if (!sbAppendValue(builder, value, true)) return false;
  }

  return sbAppend(builder, first ? "}" : " }", first ? 1 : 2);
}

/* Every value that reports as a function, whatever shape it has underneath. */
#define IS_CALLABLE(v)                                                    \
  (IS_NATIVE(v) || IS_FUNCTION(v) || IS_CLOSURE(v) || IS_BOUND_METHOD(v) || \
   IS_CLASS(v))

/* `[Function: name]`, `[Function (anonymous)]` or `[class Name extends Base]`,
 * matching what Node prints — which is the only reason to prefer any of these
 * over a bare placeholder.
 *
 * Built on the heap rather than in a fixed buffer because a name can be any
 * length. */
static char *renderCallable(Value value, size_t *lengthOut) {
  ObjString *name = NULL;
  ObjClass *klass = NULL;

  if (IS_NATIVE(value)) {
    name = AS_NATIVE(value)->name;
  } else if (IS_FUNCTION(value)) {
    name = AS_FUNCTION(value)->name;
  } else if (IS_CLOSURE(value)) {
    name = AS_CLOSURE(value)->function->name;
  } else if (IS_BOUND_METHOD(value)) {
    Obj *method = AS_BOUND_METHOD(value)->method;
    name = method->type == OBJ_NATIVE ? ((ObjNative *)method)->name
                                      : ((ObjClosure *)method)->function->name;
  } else {
    klass = AS_CLASS(value);
    name = klass->name;
  }

  StringBuilder builder = {NULL, 0, 0};
  bool ok;
  if (klass != NULL) {
    ok = sbAppend(&builder, "[class ", 7) &&
         sbAppend(&builder, name->chars, (size_t)name->length);
    if (ok && klass->superclass != NULL) {
      ok = sbAppend(&builder, " extends ", 9) &&
           sbAppend(&builder, klass->superclass->name->chars,
                    (size_t)klass->superclass->name->length);
    }
    ok = ok && sbAppend(&builder, "]", 1);
  } else if (name == NULL) {
    ok = sbAppend(&builder, "[Function (anonymous)]", 22);
  } else {
    ok = sbAppend(&builder, "[Function: ", 11) &&
         sbAppend(&builder, name->chars, (size_t)name->length) &&
         sbAppend(&builder, "]", 1);
  }

  if (!ok) {
    free(builder.data);
    return NULL;
  }
  if (lengthOut != NULL) *lengthOut = builder.length;
  return builder.data;
}

static bool sbAppendValue(StringBuilder *builder, Value value, bool quoteStrings) {
  /* `quoteStrings` marks the inspect path — inside a container, or a
   * console.log argument — where -0 is shown with its sign. */
  if (quoteStrings && IS_NUMBER(value)) {
    char buffer[32];
    int length = formatNumberEx(buffer, sizeof(buffer), AS_NUMBER(value), true);
    return sbAppend(builder, buffer, (size_t)length);
  }

  if (IS_OBJ(value)) {
    if (IS_ARRAY(value)) return sbAppendArray(builder, AS_ARRAY(value));
    if (IS_OBJECT(value)) return sbAppendObject(builder, AS_OBJECT(value));
    if (IS_PROMISE(value)) return sbAppendPromise(builder, AS_PROMISE(value));
    if (IS_STRING(value) && quoteStrings) {
      ObjString *string = AS_STRING(value);
      return sbAppend(builder, "'", 1) &&
             sbAppend(builder, string->chars, (size_t)string->length) &&
             sbAppend(builder, "'", 1);
    }
  }

  /* Only scalars reach here; containers were handled above. Recursing into
   * csValueToCString for a container would loop forever. */
  size_t length = 0;
  char *text = csValueToCString(value, &length);
  if (text == NULL) return false;
  bool ok = sbAppend(builder, text, length);
  free(text);
  return ok;
}

char *csValueInspect(Value value, size_t *lengthOut) {
  StringBuilder builder = {NULL, 0, 0};
  /* quoteStrings is false at the top level and true inside containers, which
   * sbAppendArray and sbAppendObject arrange for themselves. */
  if (!sbAppendValue(&builder, value, IS_NUMBER(value))) {
    free(builder.data);
    return NULL;
  }
  if (lengthOut != NULL) *lengthOut = builder.length;
  return builder.data;
}

char *csValueToCString(Value value, size_t *lengthOut) {
  char buffer[32];
  const char *text = buffer;
  size_t length;

  if (IS_NUMBER(value)) {
    length = (size_t)formatNumber(buffer, sizeof(buffer), AS_NUMBER(value));
  } else if (IS_BOOL(value)) {
    text = AS_BOOL(value) ? "true" : "false";
    length = AS_BOOL(value) ? 4 : 5;
  } else if (IS_NULL(value)) {
    text = "null";
    length = 4;
  } else if (IS_UNDEFINED(value)) {
    text = "undefined";
    length = 9;
  } else {
      if (IS_STRING(value)) {
        ObjString *string = AS_STRING(value);
        text = string->chars;
        length = (size_t)string->length;
      } else if (IS_CALLABLE(value)) {
        return renderCallable(value, lengthOut);
      } else {
        /* Arrays and objects are rendered structurally, so console.log shows
         * their contents rather than a placeholder. */
        StringBuilder builder = {NULL, 0, 0};
        if (!sbAppendValue(&builder, value, false)) {
          free(builder.data);
          return NULL;
        }
        if (lengthOut != NULL) *lengthOut = builder.length;
        return builder.data;
      }
  }

  char *result = (char *)malloc(length + 1);
  if (result == NULL) return NULL;
  memcpy(result, text, length);
  result[length] = '\0';
  if (lengthOut != NULL) *lengthOut = length;
  return result;
}
