/* native_number.c — the methods a number answers to.
 *
 * A number is a primitive here rather than an object, so these live in a table
 * the VM picks by receiver type, exactly as the string and array methods do.
 * All three produce text: the interesting question about a number at run time
 * is almost always how to write it down.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

static bool digitsArg(int argCount, Value *args, int index, int fallback,
                      const char *method, int low, int high, int *out) {
  if (argCount <= index) {
    *out = fallback;
    return true;
  }
  if (!IS_NUMBER(args[index])) {
    csVMRuntimeError("%s expects a number", method);
    return false;
  }
  double raw = AS_NUMBER(args[index]);
  if (raw < low || raw > high || raw != raw) {
    csVMRuntimeError("%s expects a value between %d and %d, got %g", method, low,
                     high, raw);
    return false;
  }
  *out = (int)raw;
  return true;
}

static bool numberToFixed(Value receiver, int argCount, Value *args, Value *result) {
  int digits;
  if (!digitsArg(argCount, args, 0, 0, "toFixed", 0, 100, &digits)) return false;

  char buffer[512];
  int length = snprintf(buffer, sizeof buffer, "%.*f", digits, AS_NUMBER(receiver));
  if (length < 0 || length >= (int)sizeof buffer) {
    csVMRuntimeError("toFixed produced more text than it can hold");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(buffer, length));
  return true;
}

static bool numberToPrecision(Value receiver, int argCount, Value *args,
                              Value *result) {
  /* With no argument it is `toString`, which is what the specification says
   * and is easy to get wrong by treating the default as zero. */
  if (argCount == 0) {
    size_t length = 0;
    char *text = csValueToCString(receiver, &length);
    if (text == NULL) {
      csVMRuntimeError("out of memory converting a number");
      return false;
    }
    *result = OBJ_VAL(csStringCopy(text, (int)length));
    free(text);
    return true;
  }

  int digits;
  if (!digitsArg(argCount, args, 0, 6, "toPrecision", 1, 100, &digits)) return false;

  char buffer[512];
  int length = snprintf(buffer, sizeof buffer, "%.*g", digits, AS_NUMBER(receiver));
  if (length < 0 || length >= (int)sizeof buffer) {
    csVMRuntimeError("toPrecision produced more text than it can hold");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(buffer, length));
  return true;
}

/* `(255).toString(16)` — the only one of the three that is not decimal, and
 * the reason this method is worth having at all. */
static bool numberToString(Value receiver, int argCount, Value *args, Value *result) {
  int radix;
  if (!digitsArg(argCount, args, 0, 10, "toString", 2, 36, &radix)) return false;

  double value = AS_NUMBER(receiver);
  if (radix == 10 || value != value || value == HUGE_VAL || value == -HUGE_VAL) {
    size_t length = 0;
    char *text = csValueToCString(receiver, &length);
    if (text == NULL) {
      csVMRuntimeError("out of memory converting a number");
      return false;
    }
    *result = OBJ_VAL(csStringCopy(text, (int)length));
    free(text);
    return true;
  }

  /* Only the integer part, as JavaScript's own output does for a radix other
   * than ten in every case worth relying on. */
  bool negative = value < 0;
  long long whole = (long long)(negative ? -value : value);

  char digits[80];
  int at = (int)sizeof digits;
  digits[--at] = '\0';
  if (whole == 0) digits[--at] = '0';
  while (whole > 0 && at > 0) {
    int digit = (int)(whole % radix);
    digits[--at] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    whole /= radix;
  }
  if (negative && at > 0) digits[--at] = '-';

  *result = OBJ_VAL(csStringCopy(digits + at, (int)(sizeof digits - 1 - (size_t)at)));
  return true;
}

static void defineNumberMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.numberMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csNumberMethodsInstall(void) {
  defineNumberMethod("toFixed", numberToFixed, -1);
  defineNumberMethod("toPrecision", numberToPrecision, -1);
  defineNumberMethod("toString", numberToString, -1);
}
