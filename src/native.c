#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* Writes the arguments separated by spaces, the way console.log does. */
static void writeArgs(FILE *out, int argCount, Value *args) {
  for (int i = 0; i < argCount; i++) {
    if (i > 0) fputc(' ', out);
    size_t length = 0;
    char *text = csValueToCString(args[i], &length);
    if (text == NULL) continue;
    fwrite(text, 1, length, out);
    free(text);
  }
  fputc('\n', out);
}

static bool consoleLog(int argCount, Value *args, Value *result) {
  writeArgs(stdout, argCount, args);
  *result = UNDEFINED_VAL;
  return true;
}

static bool consoleError(int argCount, Value *args, Value *result) {
  writeArgs(stderr, argCount, args);
  *result = UNDEFINED_VAL;
  return true;
}

static bool mathFloor(int argCount, Value *args, Value *result) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    csVMRuntimeError("Math.floor expects one number");
    return false;
  }
  *result = NUMBER_VAL(floor(AS_NUMBER(args[0])));
  return true;
}

static bool mathAbs(int argCount, Value *args, Value *result) {
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    csVMRuntimeError("Math.abs expects one number");
    return false;
  }
  *result = NUMBER_VAL(fabs(AS_NUMBER(args[0])));
  return true;
}

static bool mathMax(int argCount, Value *args, Value *result) {
  if (argCount == 0) {
    *result = NUMBER_VAL(-INFINITY);
    return true;
  }
  double best = -INFINITY;
  for (int i = 0; i < argCount; i++) {
    if (!IS_NUMBER(args[i])) {
      csVMRuntimeError("Math.max expects numbers");
      return false;
    }
    if (AS_NUMBER(args[i]) > best) best = AS_NUMBER(args[i]);
  }
  *result = NUMBER_VAL(best);
  return true;
}

static bool mathMin(int argCount, Value *args, Value *result) {
  if (argCount == 0) {
    *result = NUMBER_VAL(INFINITY);
    return true;
  }
  double best = INFINITY;
  for (int i = 0; i < argCount; i++) {
    if (!IS_NUMBER(args[i])) {
      csVMRuntimeError("Math.min expects numbers");
      return false;
    }
    if (AS_NUMBER(args[i]) < best) best = AS_NUMBER(args[i]);
  }
  *result = NUMBER_VAL(best);
  return true;
}

/* Number(x) — the explicit conversion that replaces JavaScript's unary '+'. */
static bool numberConvert(int argCount, Value *args, Value *result) {
  if (argCount != 1) {
    csVMRuntimeError("Number expects exactly one argument");
    return false;
  }
  *result = NUMBER_VAL(csValueToNumber(args[0]));
  return true;
}

/* String(x) — the explicit conversion that replaces `"" + x`. */
static bool stringConvert(int argCount, Value *args, Value *result) {
  if (argCount != 1) {
    csVMRuntimeError("String expects exactly one argument");
    return false;
  }
  size_t length = 0;
  char *text = csValueToCString(args[0], &length);
  if (text == NULL) {
    csVMRuntimeError("out of memory converting to string");
    return false;
  }
  ObjString *string = csStringCopy(text, (int)length);
  free(text);
  *result = OBJ_VAL(string);
  return true;
}

static bool booleanConvert(int argCount, Value *args, Value *result) {
  if (argCount != 1) {
    csVMRuntimeError("Boolean expects exactly one argument");
    return false;
  }
  *result = BOOL_VAL(csValueIsTruthy(args[0]));
  return true;
}

/* Defines a global, keeping the value rooted across the table insert. */
static void defineGlobal(const char *name, Value value) {
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.globals, key, value);
  /* Built-ins are constants: reassigning `console` should fail loudly rather
   * than leave the program with no way to print. */
  csVMMarkGlobalConst(key);
  csPopTempRoot();
  if (IS_OBJ(value)) csPopTempRoot();
}

/* Builds a namespace object and installs it as a global. */
static ObjObject *defineNamespace(const char *name) {
  ObjObject *object = csObjectNew(name);
  csPushTempRoot((Obj *)object);
  defineGlobal(name, OBJ_VAL(object));
  csPopTempRoot();
  return object;
}

static void defineMethod(ObjObject *object, const char *name, NativeFn function,
                         int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  csObjectSetProperty(object, name, OBJ_VAL(native));
  csPopTempRoot();
}

static void defineFunction(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  defineGlobal(name, OBJ_VAL(native));
  csPopTempRoot();
}

void csNativesInstall(void) {
  ObjObject *console = defineNamespace("console");
  defineMethod(console, "log", consoleLog, -1);
  defineMethod(console, "error", consoleError, -1);
  defineMethod(console, "warn", consoleError, -1);

  ObjObject *mathObject = defineNamespace("Math");
  defineMethod(mathObject, "floor", mathFloor, 1);
  defineMethod(mathObject, "abs", mathAbs, 1);
  defineMethod(mathObject, "max", mathMax, -1);
  defineMethod(mathObject, "min", mathMin, -1);
  csObjectSetProperty(mathObject, "PI", NUMBER_VAL(3.14159265358979323846));
  csObjectSetProperty(mathObject, "E", NUMBER_VAL(2.71828182845904523536));

  /* Explicit conversions, so nothing has to rely on implicit coercion. */
  defineFunction("Number", numberConvert, 1);
  defineFunction("String", stringConvert, 1);
  defineFunction("Boolean", booleanConvert, 1);

  defineGlobal("NaN", NUMBER_VAL(NAN));
  defineGlobal("Infinity", NUMBER_VAL(INFINITY));
}
