/* native_convert.c — converting to a number, a string or a boolean, and parsing.
 *
 * `Number(x)` and `parseInt(x)` are different operations and CScript keeps
 * them apart: the first is a conversion the type checker knows about, the
 * second reads as much of a string as looks numeric and stops.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"
#include "runtime/vm_internal.h"

#include "native/native_internal.h"

static bool numberConvert(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1) {
    csVMRuntimeError("Number expects exactly one argument");
    return false;
  }
  *result = NUMBER_VAL(csValueToNumber(args[0]));
  return true;
}

/* String(x) — the explicit conversion that replaces `"" + x`. */
static bool stringConvert(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1) {
    csVMRuntimeError("String expects exactly one argument");
    return false;
  }
  size_t length = 0;
  char *text = csVMValueToText(args[0], &length);
  if (text == NULL) {
    /* A failing user `toString` has already reported why. */
    if (!vm.hasPendingException) {
      csVMRuntimeError("out of memory converting to string");
    }
    return false;
  }
  ObjString *string = csStringCopy(text, (int)length);
  free(text);
  *result = OBJ_VAL(string);
  return true;
}

static bool booleanConvert(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1) {
    csVMRuntimeError("Boolean expects exactly one argument");
    return false;
  }
  *result = BOOL_VAL(csValueIsTruthy(args[0]));
  return true;
}

/* ---------------- numeric parsing ---------------- */

/* parseInt stops at the first character that is not a digit, unlike Number(),
 * which requires the whole string. */
static bool globalParseInt(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_STRING(args[0])) {
    *result = NUMBER_VAL(argCount >= 1 && IS_NUMBER(args[0])
                             ? trunc(AS_NUMBER(args[0]))
                             : NAN);
    return true;
  }

  int base = 10;
  if (argCount >= 2 && IS_NUMBER(args[1]) && AS_NUMBER(args[1]) != 0) {
    base = (int)AS_NUMBER(args[1]);
  }

  char *end = NULL;
  const char *text = AS_CSTRING(args[0]);
  long long parsed = strtoll(text, &end, base);
  *result = NUMBER_VAL(end == text ? NAN : (double)parsed);
  return true;
}

static bool globalParseFloat(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_STRING(args[0])) {
    *result = NUMBER_VAL(argCount >= 1 && IS_NUMBER(args[0]) ? AS_NUMBER(args[0]) : NAN);
    return true;
  }
  char *end = NULL;
  const char *text = AS_CSTRING(args[0]);
  double parsed = strtod(text, &end);
  *result = NUMBER_VAL(end == text ? NAN : parsed);
  return true;
}

static bool globalIsNaN(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  double value = argCount >= 1 ? csValueToNumber(args[0]) : NAN;
  *result = BOOL_VAL(isnan(value));
  return true;
}

static bool globalIsFinite(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  double value = argCount >= 1 ? csValueToNumber(args[0]) : NAN;
  *result = BOOL_VAL(!isnan(value) && !isinf(value));
  return true;
}

static bool numberIsInteger(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  bool ok = argCount >= 1 && IS_NUMBER(args[0]);
  double value = ok ? AS_NUMBER(args[0]) : NAN;
  *result = BOOL_VAL(ok && !isnan(value) && !isinf(value) && value == trunc(value));
  return true;
}

/* Number.isNaN and Number.isFinite differ from the globals by not coercing. */
static bool numberIsNaN(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  *result = BOOL_VAL(argCount >= 1 && IS_NUMBER(args[0]) && isnan(AS_NUMBER(args[0])));
  return true;
}

static bool numberIsFinite(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  bool ok = argCount >= 1 && IS_NUMBER(args[0]);
  *result = BOOL_VAL(ok && !isnan(AS_NUMBER(args[0])) && !isinf(AS_NUMBER(args[0])));
  return true;
}

/* `String.fromCharCode(72, 105)` is "Hi".
 *
 * Bytes, not UTF-16 code units: CScript's strings are bytes and its
 * `charCodeAt` answers one, so this is that operation inverted. A code above
 * 255 is refused rather than truncated — silently writing the low byte of a
 * character someone asked for is the kind of wrong answer that looks right. */
static bool stringFromCharCode(Value receiver, int argCount, Value *args,
                               Value *result) {
  (void)receiver;
  char *buffer = CS_ALLOCATE(char, argCount + 1);
  for (int i = 0; i < argCount; i++) {
    if (!IS_NUMBER(args[i])) {
      CS_FREE_ARRAY(char, buffer, argCount + 1);
      csVMRuntimeError("String.fromCharCode expects numbers");
      return false;
    }
    double code = AS_NUMBER(args[i]);
    if (code < 0 || code > 255 || code != (double)(int)code) {
      CS_FREE_ARRAY(char, buffer, argCount + 1);
      csVMRuntimeError("String.fromCharCode expects whole numbers from 0 to 255, "
                       "got %g",
                       code);
      return false;
    }
    buffer[i] = (char)(unsigned char)(int)code;
  }
  buffer[argCount] = '\0';

  ObjString *text = csStringCopy(buffer, argCount);
  CS_FREE_ARRAY(char, buffer, argCount + 1);
  *result = OBJ_VAL(text);
  return true;
}

void csNativeInstallConversions(void) {
  /* Number is callable *and* a namespace, so it is defined as a function whose
   * statics carry the rest. */
  ObjNative *numberFn = csNativeNew(numberConvert, "Number", 1);
  csPushTempRoot((Obj *)numberFn);
  ObjObject *ns = csObjectNew("Number");
  csPushTempRoot((Obj *)ns);
  numberFn->statics = ns;
  csNativeDefineGlobal("Number", OBJ_VAL(numberFn));
  csNativeDefineMethod(ns, "isInteger", numberIsInteger, 1);
  csNativeDefineMethod(ns, "isNaN", numberIsNaN, 1);
  csNativeDefineMethod(ns, "isFinite", numberIsFinite, 1);
  csNativeDefineMethod(ns, "parseInt", globalParseInt, -1);
  csNativeDefineMethod(ns, "parseFloat", globalParseFloat, -1);
  csObjectSetProperty(ns, "MAX_SAFE_INTEGER", NUMBER_VAL(9007199254740991.0));
  csObjectSetProperty(ns, "MIN_SAFE_INTEGER", NUMBER_VAL(-9007199254740991.0));
  csObjectSetProperty(ns, "EPSILON", NUMBER_VAL(2.220446049250313e-16));
  csObjectSetProperty(ns, "MAX_VALUE", NUMBER_VAL(1.7976931348623157e308));
  csObjectSetProperty(ns, "MIN_VALUE", NUMBER_VAL(5e-324));
  csObjectFreeze(ns);
  csPopTempRoot();
  csPopTempRoot();

  csNativeDefineFunction("parseInt", globalParseInt, -1);
  csNativeDefineFunction("parseFloat", globalParseFloat, -1);
  csNativeDefineFunction("isNaN", globalIsNaN, -1);
  csNativeDefineFunction("isFinite", globalIsFinite, -1);

  /* Explicit conversions, so nothing has to rely on implicit coercion.
   * `String` is callable *and* a namespace, the same shape as `Number` above:
   * building a character from its code is the other half of `charCodeAt`, and
   * without it a program cannot construct one at all. */
  ObjNative *stringFn = csNativeNew(stringConvert, "String", 1);
  csPushTempRoot((Obj *)stringFn);
  ObjObject *stringNs = csObjectNew("String");
  csPushTempRoot((Obj *)stringNs);
  stringFn->statics = stringNs;
  csNativeDefineGlobal("String", OBJ_VAL(stringFn));
  csNativeDefineMethod(stringNs, "fromCharCode", stringFromCharCode, -1);
  csObjectFreeze(stringNs);
  csPopTempRoot();
  csPopTempRoot();

  csNativeDefineFunction("Boolean", booleanConvert, 1);
}
