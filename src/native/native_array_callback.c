/* native_array_callback.c — an array's methods that call back into user code.
 *
 * Kept apart from the rest because the hazard is different, not the work: a
 * callback can allocate, so the collector can run in the middle of a `map`;
 * it can throw, so every one of these has to leave the array in a state the
 * unwinding can survive; and it can modify the array it was given, which is
 * why the length is re-read rather than cached.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#include "native/native_internal.h"

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
    csNativeAppendRooted(mapped, produced);
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
    if (csValueIsTruthy(verdict)) csNativeAppendRooted(kept, element);
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

void csNativeInstallArrayCallbacks(void) {
  csNativeDefineArrayMethod("forEach", arrayForEach, -1);
  csNativeDefineArrayMethod("map", arrayMap, -1);
  csNativeDefineArrayMethod("filter", arrayFilter, -1);
  csNativeDefineArrayMethod("reduce", arrayReduce, -1);
  csNativeDefineArrayMethod("reduceRight", arrayReduceRight, -1);
  csNativeDefineArrayMethod("find", arrayFind, -1);
  csNativeDefineArrayMethod("findIndex", arrayFindIndex, -1);
  csNativeDefineArrayMethod("some", arraySome, -1);
  csNativeDefineArrayMethod("every", arrayEvery, -1);
  csNativeDefineArrayMethod("sort", arraySort, -1);
}
