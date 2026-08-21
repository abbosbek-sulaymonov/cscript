/* native_array.c — the built-in array methods.
 *
 * Every method here receives the array as `receiver`, because OP_INVOKE leaves
 * it on the stack below the arguments rather than binding a method object.
 *
 * The higher-order ones call back into user code through csVMCallCallback,
 * which runs a nested interpreter loop. Two consequences follow and are handled
 * throughout: a callback can allocate, so any array being built has to be
 * rooted across the loop; and a callback can fail, so every call site checks.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#define ARRAY_OF(receiver) (AS_ARRAY(receiver))

/* Appends with the value protected.
 *
 * Growing the element array allocates, which can collect. A value that only
 * exists in a C local at that moment — a callback's return value, say — is
 * reachable from nothing the collector scans, so it has to be rooted across
 * the write. `make test-gc` found this the hard way. */
static void appendRooted(ObjArray *array, Value value) {
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

static bool argIndex(int argCount, Value *args, int position, int length,
                     int fallback, int *out) {
  if (argCount <= position || IS_UNDEFINED(args[position])) {
    *out = fallback;
    return true;
  }
  if (!IS_NUMBER(args[position])) {
    csVMRuntimeError("array index must be a number, got %s",
                     csValueTypeName(args[position]));
    return false;
  }
  *out = resolveIndex(AS_NUMBER(args[position]), length);
  return true;
}

static bool arrayPush(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  for (int i = 0; i < argCount; i++) appendRooted(array, args[i]);
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
  memmove(array->elements.values, array->elements.values + 1,
          sizeof(Value) * (size_t)(array->elements.count - 1));
  array->elements.count--;
  return true;
}

static bool arrayUnshift(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  /* Grow first, then slide the tail up, so the new slots exist before the move. */
  for (int i = 0; i < argCount; i++) csValueArrayWrite(&array->elements, UNDEFINED_VAL);
  memmove(array->elements.values + argCount, array->elements.values,
          sizeof(Value) * (size_t)(array->elements.count - argCount));
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
    appendRooted(copy, array->elements.values[i]);
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
    appendRooted(joined, array->elements.values[i]);
  }
  for (int i = 0; i < argCount; i++) {
    /* An array argument is flattened one level; anything else is appended. */
    if (IS_ARRAY(args[i])) {
      ObjArray *other = AS_ARRAY(args[i]);
      for (int j = 0; j < other->elements.count; j++) {
        appendRooted(joined, other->elements.values[j]);
      }
    } else {
      appendRooted(joined, args[i]);
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
      csVMRuntimeError("join separator must be a string, got %s",
                       csValueTypeName(args[0]));
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

  *result = at >= 0 && at < array->elements.count ? array->elements.values[at]
                                                  : UNDEFINED_VAL;
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
      appendRooted(out, element);
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
        appendRooted(out, pieces->elements.values[j]);
      }
    } else {
      appendRooted(out, produced);
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


/* ---------------- methods that call back into user code ---------------- */

/* Invokes `callback(element, index, array)` and writes the result.
 *
 * The callee and its arguments are pushed here and consumed by
 * csVMCallCallback, which runs a nested interpreter loop. The array itself is
 * passed as the third argument, matching JavaScript, and is what keeps it
 * reachable from the stack while the callback runs. */
static bool callWithElement(Value callback, Value element, int index, Value array,
                            Value *out) {
  Value args[3] = {element, NUMBER_VAL(index), array};
  return csVMCallAdapted(callback, args, 3, out);
}

static bool requireCallback(int argCount, Value *args, const char *method) {
  if (argCount < 1) {
    csVMRuntimeError("%s expects a callback", method);
    return false;
  }
  (void)args;
  return true;
}

static bool arrayForEach(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallback(argCount, args, "forEach")) return false;
  ObjArray *array = ARRAY_OF(receiver);

  for (int i = 0; i < array->elements.count; i++) {
    Value ignored;
    if (!callWithElement(args[0], array->elements.values[i], i, receiver, &ignored)) {
      return false;
    }
  }
  *result = UNDEFINED_VAL;
  return true;
}

static bool arrayMap(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallback(argCount, args, "map")) return false;
  ObjArray *array = ARRAY_OF(receiver);

  ObjArray *mapped = csArrayNew();
  /* The callback allocates, and the new array is reachable from nothing else
   * until it is returned — so it has to stay rooted for the whole loop. */
  csPushTempRoot((Obj *)mapped);

  for (int i = 0; i < array->elements.count; i++) {
    Value produced;
    if (!callWithElement(args[0], array->elements.values[i], i, receiver, &produced)) {
      csPopTempRoot();
      return false;
    }
    appendRooted(mapped, produced);
  }

  csPopTempRoot();
  *result = OBJ_VAL(mapped);
  return true;
}

static bool arrayFilter(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallback(argCount, args, "filter")) return false;
  ObjArray *array = ARRAY_OF(receiver);

  ObjArray *kept = csArrayNew();
  csPushTempRoot((Obj *)kept);

  for (int i = 0; i < array->elements.count; i++) {
    Value verdict;
    Value element = array->elements.values[i];
    if (!callWithElement(args[0], element, i, receiver, &verdict)) {
      csPopTempRoot();
      return false;
    }
    if (csValueIsTruthy(verdict)) appendRooted(kept, element);
  }

  csPopTempRoot();
  *result = OBJ_VAL(kept);
  return true;
}

static bool arrayReduce(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallback(argCount, args, "reduce")) return false;
  ObjArray *array = ARRAY_OF(receiver);

  int index = 0;
  Value accumulator;
  if (argCount >= 2) {
    accumulator = args[1];
  } else {
    if (array->elements.count == 0) {
      csVMRuntimeError("reduce of an empty array with no initial value");
      return false;
    }
    accumulator = array->elements.values[index++];
  }

  for (; index < array->elements.count; index++) {
    /* The accumulator is a plain C local, so it is invisible to the collector
     * while the callback runs — push it onto the stack for the duration. */
    csVMPush(accumulator);
    Value callArgs[4] = {accumulator, array->elements.values[index],
                         NUMBER_VAL(index), receiver};

    Value produced;
    if (!csVMCallAdapted(args[0], callArgs, 4, &produced)) return false;
    csVMPop(); /* the rooted accumulator */
    accumulator = produced;
  }

  *result = accumulator;
  return true;
}

/* The same fold, from the end. Written out rather than sharing a direction
 * flag with reduce: the two loops differ in three places, and a flag threaded
 * through all of them reads worse than the second loop does. */
static bool arrayReduceRight(Value receiver, int argCount, Value *args,
                             Value *result) {
  if (!requireCallback(argCount, args, "reduceRight")) return false;
  ObjArray *array = ARRAY_OF(receiver);

  int index = array->elements.count - 1;
  Value accumulator;
  if (argCount >= 2) {
    accumulator = args[1];
  } else {
    if (array->elements.count == 0) {
      csVMRuntimeError("reduceRight of an empty array with no initial value");
      return false;
    }
    accumulator = array->elements.values[index--];
  }

  for (; index >= 0; index--) {
    csVMPush(accumulator);
    Value callArgs[4] = {accumulator, array->elements.values[index],
                         NUMBER_VAL(index), receiver};

    Value produced;
    if (!csVMCallAdapted(args[0], callArgs, 4, &produced)) return false;
    csVMPop();
    accumulator = produced;
  }

  *result = accumulator;
  return true;
}

/* find/findIndex/some/every share one walk; `mode` selects what to return. */
typedef enum {
  SEARCH_FIND,
  SEARCH_FIND_INDEX,
  SEARCH_SOME,
  SEARCH_EVERY,
} SearchMode;

static bool arraySearch(Value receiver, int argCount, Value *args, Value *result,
                        SearchMode mode, const char *method) {
  if (!requireCallback(argCount, args, method)) return false;
  ObjArray *array = ARRAY_OF(receiver);

  for (int i = 0; i < array->elements.count; i++) {
    Value verdict;
    Value element = array->elements.values[i];
    if (!callWithElement(args[0], element, i, receiver, &verdict)) return false;

    bool matched = csValueIsTruthy(verdict);
    if (mode == SEARCH_EVERY) {
      if (!matched) {
        *result = BOOL_VAL(false);
        return true;
      }
      continue;
    }
    if (!matched) continue;

    switch (mode) {
      case SEARCH_FIND:       *result = element; return true;
      case SEARCH_FIND_INDEX: *result = NUMBER_VAL(i); return true;
      case SEARCH_SOME:       *result = BOOL_VAL(true); return true;
      case SEARCH_EVERY:      break;
    }
  }

  switch (mode) {
    case SEARCH_FIND:       *result = UNDEFINED_VAL; break;
    case SEARCH_FIND_INDEX: *result = NUMBER_VAL(-1); break;
    case SEARCH_SOME:       *result = BOOL_VAL(false); break;
    case SEARCH_EVERY:      *result = BOOL_VAL(true); break;
  }
  return true;
}

static bool arrayFind(Value r, int c, Value *a, Value *out) {
  return arraySearch(r, c, a, out, SEARCH_FIND, "find");
}
static bool arrayFindIndex(Value r, int c, Value *a, Value *out) {
  return arraySearch(r, c, a, out, SEARCH_FIND_INDEX, "findIndex");
}
static bool arraySome(Value r, int c, Value *a, Value *out) {
  return arraySearch(r, c, a, out, SEARCH_SOME, "some");
}
static bool arrayEvery(Value r, int c, Value *a, Value *out) {
  return arraySearch(r, c, a, out, SEARCH_EVERY, "every");
}

/* Compares two elements the way Array.prototype.sort does by default: by their
 * string form, which is why [10, 9] sorts to [10, 9]. Surprising, but it is the
 * specified behaviour and code depends on it. */
static int compareAsStrings(Value a, Value b, bool *failed) {
  size_t leftLength = 0;
  size_t rightLength = 0;
  char *left = csValueToCString(a, &leftLength);
  char *right = csValueToCString(b, &rightLength);
  if (left == NULL || right == NULL) {
    free(left);
    free(right);
    *failed = true;
    return 0;
  }
  int order = strcmp(left, right);
  free(left);
  free(right);
  return order;
}

/* Insertion sort: stable, and the comparator may run arbitrary user code, so a
 * simple predictable number of comparisons is worth more than asymptotics on
 * the array sizes this language is used for. */
static bool arraySort(Value receiver, int argCount, Value *args, Value *result) {
  ObjArray *array = ARRAY_OF(receiver);
  bool hasComparator = argCount >= 1 && !IS_UNDEFINED(args[0]);

  for (int i = 1; i < array->elements.count; i++) {
    Value key = array->elements.values[i];
    int j = i - 1;

    while (j >= 0) {
      int order;
      if (hasComparator) {
        /* `key` lives only in a C local, so root it across the call. */
        csVMPush(key);
        Value callArgs[2] = {array->elements.values[j], key};

        Value verdict;
        if (!csVMCallAdapted(args[0], callArgs, 2, &verdict)) return false;
        key = csVMPop();

        if (!IS_NUMBER(verdict)) {
          csVMRuntimeError("sort comparator must return a number, got %s",
                           csValueTypeName(verdict));
          return false;
        }
        order = AS_NUMBER(verdict) > 0 ? 1 : (AS_NUMBER(verdict) < 0 ? -1 : 0);
      } else {
        bool failed = false;
        order = compareAsStrings(array->elements.values[j], key, &failed);
        if (failed) {
          csVMRuntimeError("out of memory while sorting");
          return false;
        }
      }

      if (order <= 0) break;
      array->elements.values[j + 1] = array->elements.values[j];
      j--;
    }
    array->elements.values[j + 1] = key;
  }

  *result = receiver;
  return true;
}

/* ---------------- installation ---------------- */

static void defineArrayMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.arrayMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csArrayMethodsInstall(void) {
  defineArrayMethod("push", arrayPush, -1);
  defineArrayMethod("pop", arrayPop, 0);
  defineArrayMethod("shift", arrayShift, 0);
  defineArrayMethod("unshift", arrayUnshift, -1);
  defineArrayMethod("slice", arraySlice, -1);
  defineArrayMethod("concat", arrayConcat, -1);
  defineArrayMethod("join", arrayJoin, -1);
  defineArrayMethod("indexOf", arrayIndexOf, -1);
  defineArrayMethod("lastIndexOf", arrayLastIndexOf, -1);
  defineArrayMethod("includes", arrayIncludes, -1);
  defineArrayMethod("reverse", arrayReverse, 0);
  defineArrayMethod("fill", arrayFill, -1);
  defineArrayMethod("sort", arraySort, -1);

  defineArrayMethod("forEach", arrayForEach, -1);
  defineArrayMethod("map", arrayMap, -1);
  defineArrayMethod("filter", arrayFilter, -1);
  defineArrayMethod("reduce", arrayReduce, -1);
  defineArrayMethod("reduceRight", arrayReduceRight, -1);
  defineArrayMethod("find", arrayFind, -1);
  defineArrayMethod("findIndex", arrayFindIndex, -1);
  defineArrayMethod("some", arraySome, -1);
  defineArrayMethod("every", arrayEvery, -1);
  defineArrayMethod("at", arrayAt, -1);
  defineArrayMethod("flat", arrayFlat, -1);
  defineArrayMethod("flatMap", arrayFlatMap, -1);
}
