/* native_array.c — an array's methods that stay inside the runtime.
 *
 * Everything here rearranges elements or reads them; nothing calls back into
 * CScript, which is what makes them all safe to write as straight loops. The
 * ones that do take a callback are in native_array_callback.c, because a call
 * can collect, throw, or reenter — and every one of them has to be written as
 * though it will.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#include "native/native_internal.h"

void csNativeAppendRooted(ObjArray *array, Value value) {
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  csValueArrayWrite(&array->elements, value);
  if (IS_OBJ(value)) csPopTempRoot();
}

/* Clamps a possibly-negative index the way JavaScript's slice-like methods do:
 * negative counts back from the end, and the result is pinned to [0, length]. */
static int resolveIndex(double raw, int length) {
  int index = (int)raw;
  if (index < 0) index += length;
  if (index < 0) return 0;
  if (index > length) return length;
  return index;
}

static bool argIndex(int argCount, Value *args, int position, int length, int fallback, int *out) {
  if (argCount <= position || IS_UNDEFINED(args[position])) {
    *out = fallback;
    return true;
  }
  if (!IS_NUMBER(args[position])) {
    csVMRuntimeError("array index must be a number, got %s", csValueTypeName(args[position]));
    return false;
  }
  *out = resolveIndex(AS_NUMBER(args[position]), length);
  return true;
}

static bool arrayPush(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  for (int i = 0; i < argCount; i++) csNativeAppendRooted(array, args[i]);
  *result = NUMBER_VAL(array->elements.count);
  return true;
}

static bool arrayPop(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ObjArray *array = ARRAY_OF(receiver);
  if (array->elements.count == 0) {
    *result = UNDEFINED_VAL;
    return true;
  }
  *result = array->elements.values[--array->elements.count];
  return true;
}

static bool arrayShift(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ObjArray *array = ARRAY_OF(receiver);
  if (array->elements.count == 0) {
    *result = UNDEFINED_VAL;
    return true;
  }
  *result = array->elements.values[0];
  memmove(array->elements.values, array->elements.values + 1, sizeof(Value) * (size_t)(array->elements.count - 1));
  array->elements.count--;
  return true;
}

static bool arrayUnshift(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  /* Grow first, then slide the tail up, so the new slots exist before the move. */
  for (int i = 0; i < argCount; i++) csValueArrayWrite(&array->elements, UNDEFINED_VAL);
  memmove(array->elements.values + argCount, array->elements.values, sizeof(Value) * (size_t)(array->elements.count - argCount));
  for (int i = 0; i < argCount; i++) array->elements.values[i] = args[i];
  *result = NUMBER_VAL(array->elements.count);
  return true;
}

static bool arraySlice(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  int length = array->elements.count;

  int start, end;
  if (!argIndex(argCount, args, 0, length, 0, &start)) return false;
  if (!argIndex(argCount, args, 1, length, length, &end)) return false;

  ObjArray *copy = csArrayNew();
  csPushTempRoot((Obj *)copy);
  for (int i = start; i < end; i++) {
    csNativeAppendRooted(copy, array->elements.values[i]);
  }
  csPopTempRoot();

  *result = OBJ_VAL(copy);
  return true;
}

static bool arrayConcat(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);

  ObjArray *joined = csArrayNew();
  csPushTempRoot((Obj *)joined);
  for (int i = 0; i < array->elements.count; i++) {
    csNativeAppendRooted(joined, array->elements.values[i]);
  }
  for (int i = 0; i < argCount; i++) {
    /* An array argument is flattened one level; anything else is appended. */
    if (IS_ARRAY(args[i])) {
      ObjArray *other = AS_ARRAY(args[i]);
      for (int j = 0; j < other->elements.count; j++) {
        csNativeAppendRooted(joined, other->elements.values[j]);
      }
    } else {
      csNativeAppendRooted(joined, args[i]);
    }
  }
  csPopTempRoot();

  *result = OBJ_VAL(joined);
  return true;
}

static bool arrayJoin(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);

  const char *separator = ",";
  size_t separatorLength = 1;
  if (argCount >= 1 && !IS_UNDEFINED(args[0])) {
    if (!IS_STRING(args[0])) {
      csVMRuntimeError("join separator must be a string, got %s", csValueTypeName(args[0]));
      return false;
    }
    separator = AS_CSTRING(args[0]);
    separatorLength = (size_t)AS_STRING(args[0])->length;
  }

  size_t capacity = 64;
  size_t length = 0;
  char *buffer = (char *)malloc(capacity);
  if (buffer == NULL) {
    csVMRuntimeError("out of memory joining an array");
    return false;
  }

  for (int i = 0; i < array->elements.count; i++) {
    Value element = array->elements.values[i];
    /* null and undefined render as empty, which is what JavaScript does. */
    size_t pieceLength = 0;
    char *piece = NULL;
    if (!IS_NULL(element) && !IS_UNDEFINED(element)) {
      piece = csValueToCString(element, &pieceLength);
      if (piece == NULL) {
        free(buffer);
        csVMRuntimeError("out of memory joining an array");
        return false;
      }
    }

    size_t needed = length + pieceLength + (i > 0 ? separatorLength : 0) + 1;
    if (needed > capacity) {
      while (capacity < needed) capacity *= 2;
      char *grown = (char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(piece);
        free(buffer);
        csVMRuntimeError("out of memory joining an array");
        return false;
      }
      buffer = grown;
    }

    if (i > 0) {
      memcpy(buffer + length, separator, separatorLength);
      length += separatorLength;
    }
    if (piece != NULL) {
      memcpy(buffer + length, piece, pieceLength);
      length += pieceLength;
      free(piece);
    }
  }
  buffer[length] = '\0';

  ObjString *joined = csStringCopy(buffer, (int)length);
  free(buffer);
  *result = OBJ_VAL(joined);
  return true;
}

static bool arrayIndexOf(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  Value needle = argCount >= 1 ? args[0] : UNDEFINED_VAL;

  for (int i = 0; i < array->elements.count; i++) {
    if (csValuesStrictEqual(array->elements.values[i], needle)) {
      *result = NUMBER_VAL(i);
      return true;
    }
  }
  *result = NUMBER_VAL(-1);
  return true;
}

static bool arrayLastIndexOf(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  Value needle = argCount >= 1 ? args[0] : UNDEFINED_VAL;

  for (int i = array->elements.count - 1; i >= 0; i--) {
    if (csValuesStrictEqual(array->elements.values[i], needle)) {
      *result = NUMBER_VAL(i);
      return true;
    }
  }
  *result = NUMBER_VAL(-1);
  return true;
}

/* `at` counts from the end when the index is negative, which is the whole
 * reason it exists beside plain subscripting. */
static bool arrayAt(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  double raw = argCount > 0 && IS_NUMBER(args[0]) ? AS_NUMBER(args[0]) : 0;
  int at = (int)raw;
  if (at < 0) at += array->elements.count;

  *result = at >= 0 && at < array->elements.count ? array->elements.values[at] : UNDEFINED_VAL;
  return true;
}

/* One level of flattening per `depth`, defaulting to one. Recursive rather
 * than iterative because the depth is the recursion. */
static void flattenInto(ObjArray *out, ObjArray *from, int depth) {
  for (int i = 0; i < from->elements.count; i++) {
    Value element = from->elements.values[i];
    if (depth > 0 && IS_ARRAY(element)) {
      flattenInto(out, AS_ARRAY(element), depth - 1);
    } else {
      csNativeAppendRooted(out, element);
    }
  }
}

static bool arrayFlat(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  int depth = argCount > 0 && IS_NUMBER(args[0]) ? (int)AS_NUMBER(args[0]) : 1;
  if (depth < 0) depth = 0;

  ObjArray *out = csArrayNew();
  csPushTempRoot((Obj *)out);
  flattenInto(out, array, depth);
  csPopTempRoot();

  *result = OBJ_VAL(out);
  return true;
}

static bool arrayFlatMap(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  if (argCount < 1 || !csValueIsCallable(args[0])) {
    csVMRuntimeError("flatMap expects a function");
    return false;
  }

  ObjArray *out = csArrayNew();
  csPushTempRoot((Obj *)out);
  for (int i = 0; i < array->elements.count; i++) {
    Value argv[3] = {array->elements.values[i], NUMBER_VAL(i), receiver};
    Value produced;
    if (!csVMCallAdapted(args[0], argv, 3, &produced)) {
      csPopTempRoot();
      return false;
    }
    /* One level only, which is what distinguishes it from map plus flat.
     *
     * The callback's result is reachable from nothing else, and appending can
     * grow the output array and collect — which freed it half way through
     * copying out of it. */
    if (IS_OBJ(produced)) csPushTempRoot(AS_OBJ(produced));
    if (IS_ARRAY(produced)) {
      ObjArray *pieces = AS_ARRAY(produced);
      for (int j = 0; j < pieces->elements.count; j++) {
        csNativeAppendRooted(out, pieces->elements.values[j]);
      }
    } else {
      csNativeAppendRooted(out, produced);
    }
    if (IS_OBJ(produced)) csPopTempRoot();
  }
  csPopTempRoot();

  *result = OBJ_VAL(out);
  return true;
}

static bool arrayIncludes(Value receiver, int argCount, Value *args, Value *result) {
  Value found;
  if (!arrayIndexOf(receiver, argCount, args, &found)) return false;
  *result = BOOL_VAL(AS_NUMBER(found) >= 0);
  return true;
}

static bool arrayReverse(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  ObjArray *array = ARRAY_OF(receiver);
  /* Reverses in place and returns the same array, as JavaScript does. */
  for (int i = 0, j = array->elements.count - 1; i < j; i++, j--) {
    Value swap = array->elements.values[i];
    array->elements.values[i] = array->elements.values[j];
    array->elements.values[j] = swap;
  }
  *result = receiver;
  return true;
}

static bool arrayFill(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  int length = array->elements.count;
  Value filler = argCount >= 1 ? args[0] : UNDEFINED_VAL;

  int start, end;
  if (!argIndex(argCount, args, 1, length, 0, &start)) return false;
  if (!argIndex(argCount, args, 2, length, length, &end)) return false;

  for (int i = start; i < end; i++) array->elements.values[i] = filler;
  *result = receiver;
  return true;
}

/* ---------------- installation ---------------- */

void csNativeDefineArrayMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.arrayMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csArrayMethodsInstall(void) {
  csNativeDefineArrayMethod("push", arrayPush, -1);
  csNativeDefineArrayMethod("pop", arrayPop, 0);
  csNativeDefineArrayMethod("shift", arrayShift, 0);
  csNativeDefineArrayMethod("unshift", arrayUnshift, -1);
  csNativeDefineArrayMethod("slice", arraySlice, -1);
  csNativeDefineArrayMethod("concat", arrayConcat, -1);
  csNativeDefineArrayMethod("join", arrayJoin, -1);
  csNativeDefineArrayMethod("indexOf", arrayIndexOf, -1);
  csNativeDefineArrayMethod("lastIndexOf", arrayLastIndexOf, -1);
  csNativeDefineArrayMethod("includes", arrayIncludes, -1);
  csNativeDefineArrayMethod("reverse", arrayReverse, 0);
  csNativeDefineArrayMethod("fill", arrayFill, -1);
  csNativeDefineArrayMethod("at", arrayAt, -1);
  csNativeDefineArrayMethod("flat", arrayFlat, -1);
  csNativeDefineArrayMethod("flatMap", arrayFlatMap, -1);

  csNativeInstallArrayCallbacks();
}
