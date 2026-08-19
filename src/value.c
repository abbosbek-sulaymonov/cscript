#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
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
  if (a.type != b.type) return false;

  switch (a.type) {
    case VAL_UNDEFINED:
    case VAL_NULL:
      return true;
    case VAL_BOOL:
      return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NUMBER:
      /* NaN !== NaN, per IEEE 754 and JS. The == does that for free. */
      return AS_NUMBER(a) == AS_NUMBER(b);
    case VAL_OBJ:
      /* Strings are interned, so pointer identity is value equality. */
      return AS_OBJ(a) == AS_OBJ(b);
  }
  return false;
}

/* Coerces a value to a number the way JS's abstract ToNumber does. This is only
 * ever reached through the Number() built-in — no operator applies it. */
double csValueToNumber(Value value) {
  switch (value.type) {
    case VAL_UNDEFINED: return NAN;
    case VAL_NULL:      return 0;
    case VAL_BOOL:      return AS_BOOL(value) ? 1 : 0;
    case VAL_NUMBER:    return AS_NUMBER(value);
    case VAL_OBJ: {
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
  }
  return NAN;
}

bool csValueIsTruthy(Value value) {
  switch (value.type) {
    case VAL_UNDEFINED:
    case VAL_NULL:
      return false;
    case VAL_BOOL:
      return AS_BOOL(value);
    case VAL_NUMBER: {
      double n = AS_NUMBER(value);
      return n != 0 && !isnan(n); /* 0, -0 and NaN are falsy */
    }
    case VAL_OBJ:
      if (IS_STRING(value)) return AS_STRING(value)->length > 0;
      return true;
  }
  return false;
}

const char *csValueTypeName(Value value) {
  switch (value.type) {
    case VAL_UNDEFINED: return "undefined";
    /* JavaScript reports "object" here. That is a bug from 1995 that cannot be
     * fixed without breaking the web; CScript is not bound by that. */
    case VAL_NULL:      return "null";
    case VAL_BOOL:      return "boolean";
    case VAL_NUMBER:    return "number";
    case VAL_OBJ:
      if (IS_STRING(value)) return "string";
      if (IS_NATIVE(value)) return "function";
      return "object";
  }
  return "undefined";
}

/* Formats a double the way JS does: integers without a decimal point, and the
 * shortest representation that round-trips otherwise. */
static int formatNumber(char *buffer, size_t size, double value) {
  if (isnan(value)) return snprintf(buffer, size, "NaN");
  if (isinf(value)) return snprintf(buffer, size, value > 0 ? "Infinity" : "-Infinity");
  if (value == 0) return snprintf(buffer, size, "0"); /* prints -0 as 0, like JS */

  if (value == (long long)value && fabs(value) < 1e15) {
    return snprintf(buffer, size, "%lld", (long long)value);
  }
  return snprintf(buffer, size, "%.17g", value);
}

void csValuePrint(Value value) {
  switch (value.type) {
    case VAL_UNDEFINED:
      printf("undefined");
      break;
    case VAL_NULL:
      printf("null");
      break;
    case VAL_BOOL:
      printf(AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NUMBER: {
      char buffer[32];
      formatNumber(buffer, sizeof(buffer), AS_NUMBER(value));
      printf("%s", buffer);
      break;
    }
    case VAL_OBJ:
      csObjectPrint(value);
      break;
  }
}

char *csValueToCString(Value value, size_t *lengthOut) {
  char buffer[32];
  const char *text = buffer;
  size_t length;

  switch (value.type) {
    case VAL_UNDEFINED: text = "undefined"; length = 9; break;
    case VAL_NULL:      text = "null";      length = 4; break;
    case VAL_BOOL:
      text = AS_BOOL(value) ? "true" : "false";
      length = AS_BOOL(value) ? 4 : 5;
      break;
    case VAL_NUMBER:
      length = (size_t)formatNumber(buffer, sizeof(buffer), AS_NUMBER(value));
      break;
    case VAL_OBJ:
      if (IS_STRING(value)) {
        ObjString *string = AS_STRING(value);
        text = string->chars;
        length = (size_t)string->length;
      } else if (IS_NATIVE(value)) {
        text = "[Function]";
        length = 10;
      } else {
        text = "[Object]";
        length = 8;
      }
      break;
    default:
      text = "undefined";
      length = 9;
      break;
  }

  char *result = (char *)malloc(length + 1);
  if (result == NULL) return NULL;
  memcpy(result, text, length);
  result[length] = '\0';
  if (lengthOut != NULL) *lengthOut = length;
  return result;
}
