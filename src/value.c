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
      /* Natives, user functions and closures are all callable, so `typeof`
       * cannot tell them apart — which matches JavaScript. */
      if (IS_NATIVE(value) || IS_FUNCTION(value) || IS_CLOSURE(value)) {
        return "function";
      }
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

  /* JavaScript prints the shortest decimal that reads back as the same double,
   * which is why Math.PI shows 15 digits and not 17. Widen the precision until
   * the text round-trips, then stop. */
  for (int precision = 1; precision < 17; precision++) {
    int written = snprintf(buffer, size, "%.*g", precision, value);
    if (strtod(buffer, NULL) == value) return written;
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
  if (!sbAppend(builder, "{", 1)) return false;

  bool first = true;
  for (int i = 0; i < object->keyCount; i++) {
    ObjString *key = object->keys[i];
    Value value;
    if (!csTableGet(&object->properties, key, &value)) continue;

    if (!sbAppend(builder, first ? " " : ", ", first ? 1 : 2)) return false;
    first = false;
    if (!sbAppend(builder, key->chars, (size_t)key->length)) return false;
    if (!sbAppend(builder, ": ", 2)) return false;
    if (!sbAppendValue(builder, value, true)) return false;
  }

  return sbAppend(builder, first ? "}" : " }", first ? 1 : 2);
}

static bool sbAppendValue(StringBuilder *builder, Value value, bool quoteStrings) {
  if (IS_OBJ(value)) {
    if (IS_ARRAY(value)) return sbAppendArray(builder, AS_ARRAY(value));
    if (IS_OBJECT(value)) return sbAppendObject(builder, AS_OBJECT(value));
    if (IS_STRING(value) && quoteStrings) {
      ObjString *string = AS_STRING(value);
      return sbAppend(builder, "'", 1) &&
             sbAppend(builder, string->chars, (size_t)string->length) &&
             sbAppend(builder, "'", 1);
    }
  }

  size_t length = 0;
  char *text = csValueToCString(value, &length);
  if (text == NULL) return false;
  bool ok = sbAppend(builder, text, length);
  free(text);
  return ok;
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
      } else if (IS_NATIVE(value) || IS_FUNCTION(value) || IS_CLOSURE(value)) {
        text = "[Function]";
        length = 10;
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
