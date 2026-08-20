#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* Writes the arguments separated by spaces, the way console.log does.
 *
 * This is the inspect path rather than the string-conversion one, so a bare
 * -0 keeps its sign and strings nested in a container are quoted — matching
 * what Node prints, which is not the same as what String() returns. */
static void writeArgs(FILE *out, int argCount, Value *args) {
  for (int i = 0; i < argCount; i++) {
    if (i > 0) fputc(' ', out);

    if (IS_STRING(args[i])) {
      /* A top-level string argument is printed bare, not quoted. */
      ObjString *string = AS_STRING(args[i]);
      fwrite(string->chars, 1, (size_t)string->length, out);
      continue;
    }

    size_t length = 0;
    char *text = csValueInspect(args[i], &length);
    if (text == NULL) continue;
    fwrite(text, 1, length, out);
    free(text);
  }
  fputc('\n', out);
}

static bool consoleLog(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  writeArgs(stdout, argCount, args);
  *result = UNDEFINED_VAL;
  return true;
}

static bool consoleError(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  writeArgs(stderr, argCount, args);
  *result = UNDEFINED_VAL;
  return true;
}

static bool mathFloor(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    csVMRuntimeError("Math.floor expects one number");
    return false;
  }
  *result = NUMBER_VAL(floor(AS_NUMBER(args[0])));
  return true;
}

static bool mathAbs(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    csVMRuntimeError("Math.abs expects one number");
    return false;
  }
  *result = NUMBER_VAL(fabs(AS_NUMBER(args[0])));
  return true;
}

static bool mathMax(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
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

static bool mathMin(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
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

static bool booleanConvert(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1) {
    csVMRuntimeError("Boolean expects exactly one argument");
    return false;
  }
  *result = BOOL_VAL(csValueIsTruthy(args[0]));
  return true;
}

/* ---------------- Object ---------------- */

/* Walks an object's keys in insertion order, which is exactly why ObjObject
 * keeps that list alongside its hash table. */
static bool objectEnumerate(int argCount, Value *args, Value *result, int mode,
                            const char *method) {
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.%s expects an object, got %s", method,
                     argCount >= 1 ? csValueTypeName(args[0]) : "no argument");
    return false;
  }
  ObjObject *object = AS_OBJECT(args[0]);

  ObjArray *out = csArrayNew();
  csPushTempRoot((Obj *)out);

  for (int i = 0; i < csObjectCount(object); i++) {
    ObjString *key = csObjectKeyAt(object, i);
    Value value = csObjectValueAt(object, i);

    if (mode == 0) {
      csValueArrayWrite(&out->elements, OBJ_VAL(key));
    } else if (mode == 1) {
      if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
      csValueArrayWrite(&out->elements, value);
      if (IS_OBJ(value)) csPopTempRoot();
    } else {
      /* entries: a two-element array per property. */
      ObjArray *pair = csArrayNew();
      csPushTempRoot((Obj *)pair);
      csValueArrayWrite(&pair->elements, OBJ_VAL(key));
      csValueArrayWrite(&pair->elements, value);
      csValueArrayWrite(&out->elements, OBJ_VAL(pair));
      csPopTempRoot();
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(out);
  return true;
}

static bool objectKeys(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 0, "keys");
}
static bool objectValues(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 1, "values");
}
static bool objectEntries(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 2, "entries");
}

static bool objectAssign(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.assign expects a target object");
    return false;
  }
  ObjObject *target = AS_OBJECT(args[0]);

  for (int i = 1; i < argCount; i++) {
    if (!IS_OBJECT(args[i])) continue;
    ObjObject *source = AS_OBJECT(args[i]);
    for (int j = 0; j < csObjectCount(source); j++) {
      csObjectPut(target, csObjectKeyAt(source, j), csObjectValueAt(source, j));
    }
  }

  *result = args[0];
  return true;
}

static bool objectHasOwn(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 2 || !IS_OBJECT(args[0]) || !IS_STRING(args[1])) {
    csVMRuntimeError("Object.hasOwn expects an object and a string");
    return false;
  }
  *result = BOOL_VAL(csObjectGet(AS_OBJECT(args[0]), AS_STRING(args[1]), NULL));
  return true;
}

/* ---------------- Array ---------------- */

static bool arrayIsArray(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  *result = BOOL_VAL(argCount >= 1 && IS_ARRAY(args[0]));
  return true;
}

static bool arrayOf(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjArray *array = csArrayNew();
  csPushTempRoot((Obj *)array);
  for (int i = 0; i < argCount; i++) csValueArrayWrite(&array->elements, args[i]);
  csPopTempRoot();
  *result = OBJ_VAL(array);
  return true;
}

/* Copies an array, or explodes a string into its characters. */
static bool arrayFrom(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("Array.from expects a value");
    return false;
  }

  ObjArray *array = csArrayNew();
  csPushTempRoot((Obj *)array);

  if (IS_ARRAY(args[0])) {
    ObjArray *source = AS_ARRAY(args[0]);
    for (int i = 0; i < source->elements.count; i++) {
      csValueArrayWrite(&array->elements, source->elements.values[i]);
    }
  } else if (IS_STRING(args[0])) {
    ObjString *source = AS_STRING(args[0]);
    for (int i = 0; i < source->length; i++) {
      ObjString *piece = csStringCopy(source->chars + i, 1);
      csPushTempRoot((Obj *)piece);
      csValueArrayWrite(&array->elements, OBJ_VAL(piece));
      csPopTempRoot();
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(array);
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

/* ---------------- Math ---------------- */

/* One wrapper for every single-argument libm function. */
#define MATH_UNARY(name, expression)                                       \
  static bool math##name(Value receiver, int argCount, Value *args,        \
                         Value *result) {                                  \
    (void)receiver;                                                        \
    if (argCount != 1 || !IS_NUMBER(args[0])) {                            \
      csVMRuntimeError("Math." #name " expects one number");               \
      return false;                                                        \
    }                                                                      \
    double x = AS_NUMBER(args[0]);                                         \
    (void)x;                                                               \
    *result = NUMBER_VAL(expression);                                      \
    return true;                                                           \
  }

MATH_UNARY(Sqrt, sqrt(x))
MATH_UNARY(Cbrt, cbrt(x))
MATH_UNARY(Ceil, ceil(x))
MATH_UNARY(Trunc, trunc(x))
MATH_UNARY(Sign, x > 0 ? 1 : (x < 0 ? -1 : x))
MATH_UNARY(Log, log(x))
MATH_UNARY(Log2, log2(x))
MATH_UNARY(Log10, log10(x))
MATH_UNARY(Exp, exp(x))
MATH_UNARY(Sin, sin(x))
MATH_UNARY(Cos, cos(x))
MATH_UNARY(Tan, tan(x))
MATH_UNARY(Atan, atan(x))
MATH_UNARY(Asin, asin(x))
MATH_UNARY(Acos, acos(x))

#undef MATH_UNARY

/* JavaScript rounds half away from zero for positives but half up overall, so
 * Math.round(-0.5) is -0 rather than -1. floor(x + 0.5) gives exactly that. */
static bool mathRound(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1 || !IS_NUMBER(args[0])) {
    csVMRuntimeError("Math.round expects one number");
    return false;
  }
  double x = AS_NUMBER(args[0]);
  *result = NUMBER_VAL(isnan(x) || isinf(x) ? x : floor(x + 0.5));
  return true;
}

static bool mathPow(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    csVMRuntimeError("Math.pow expects two numbers");
    return false;
  }
  *result = NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
  return true;
}

static bool mathAtan2(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    csVMRuntimeError("Math.atan2 expects two numbers");
    return false;
  }
  *result = NUMBER_VAL(atan2(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
  return true;
}

static bool mathHypot(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  double sum = 0;
  for (int i = 0; i < argCount; i++) {
    if (!IS_NUMBER(args[i])) {
      csVMRuntimeError("Math.hypot expects numbers");
      return false;
    }
    sum += AS_NUMBER(args[i]) * AS_NUMBER(args[i]);
  }
  *result = NUMBER_VAL(sqrt(sum));
  return true;
}

/* Not cryptographic. Seeded once from the clock at startup. */
static bool mathRandom(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  (void)argCount;
  (void)args;
  *result = NUMBER_VAL((double)rand() / ((double)RAND_MAX + 1.0));
  return true;
}

/* Error(message) — a plain object with `name` and `message`.
 *
 * `new` and classes do not exist yet, so this is a function rather than a
 * constructor, and `throw` accepts any value regardless. Having a conventional
 * shape matters mainly so `e.message` works on a caught value. */
static bool errorConstruct(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjObject *error = csObjectNew("Error");
  csPushTempRoot((Obj *)error);

  ObjString *name = csStringCopy("Error", 5);
  csPushTempRoot((Obj *)name);
  csObjectSetProperty(error, "name", OBJ_VAL(name));
  csPopTempRoot();

  Value message = argCount >= 1 ? args[0] : OBJ_VAL(csStringCopy("", 0));
  csObjectSetProperty(error, "message", message);

  csPopTempRoot();
  *result = OBJ_VAL(error);
  return true;
}

/* AggregateError(errors, message) — what `Promise.any` rejects with when every
 * input rejected, and a global in its own right.
 *
 * Same shape as Error with one more field: `errors`, the reasons in the order
 * their promises were given. Without it `any` would have to throw away exactly
 * the information the caller wanted. */
Value csAggregateError(ObjArray *errors) {
  ObjObject *error = csObjectNew("AggregateError");
  csPushTempRoot((Obj *)error);
  csPushTempRoot((Obj *)errors);

  ObjString *name = csStringCopy("AggregateError", 14);
  csPushTempRoot((Obj *)name);
  csObjectSetProperty(error, "name", OBJ_VAL(name));
  csPopTempRoot();

  ObjString *message = csStringCopy("All promises were rejected", 26);
  csPushTempRoot((Obj *)message);
  csObjectSetProperty(error, "message", OBJ_VAL(message));
  csPopTempRoot();

  csObjectSetProperty(error, "errors", OBJ_VAL(errors));

  csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(error);
}

static bool aggregateErrorConstruct(Value receiver, int argCount, Value *args,
                                    Value *result) {
  (void)receiver;
  ObjArray *errors = argCount >= 1 && IS_ARRAY(args[0]) ? AS_ARRAY(args[0])
                                                        : csArrayNew();
  csPushTempRoot((Obj *)errors);
  Value error = csAggregateError(errors);
  csPopTempRoot();

  if (argCount >= 2) {
    csPushTempRoot(AS_OBJ(error));
    csObjectSetProperty(AS_OBJECT(error), "message", args[1]);
    csPopTempRoot();
  }
  *result = error;
  return true;
}

/* Defines a global, keeping the value rooted across the table insert. */
static void defineGlobal(const char *name, Value value) {
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.builtins, key, value);
  /* Built-ins are constants: reassigning `console` should fail loudly rather
   * than leave the program with no way to print. */
  csVMMarkBuiltinConst(key);
  csPopTempRoot();
  if (IS_OBJ(value)) csPopTempRoot();
}

/* Builds a namespace object and installs it as a global. */
/* Namespaces are frozen at the end of csNativesInstall rather than here,
 * because they have no members yet. */
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
  /* Math.random is not cryptographic; one seed per process is enough. */
  srand((unsigned)time(NULL));

  ObjObject *console = defineNamespace("console");
  defineMethod(console, "log", consoleLog, -1);
  defineMethod(console, "error", consoleError, -1);
  defineMethod(console, "warn", consoleError, -1);

  ObjObject *mathObject = defineNamespace("Math");
  defineMethod(mathObject, "floor", mathFloor, 1);
  defineMethod(mathObject, "abs", mathAbs, 1);
  defineMethod(mathObject, "max", mathMax, -1);
  defineMethod(mathObject, "min", mathMin, -1);
  defineMethod(mathObject, "sqrt", mathSqrt, 1);
  defineMethod(mathObject, "cbrt", mathCbrt, 1);
  defineMethod(mathObject, "ceil", mathCeil, 1);
  defineMethod(mathObject, "trunc", mathTrunc, 1);
  defineMethod(mathObject, "sign", mathSign, 1);
  defineMethod(mathObject, "round", mathRound, 1);
  defineMethod(mathObject, "pow", mathPow, 2);
  defineMethod(mathObject, "log", mathLog, 1);
  defineMethod(mathObject, "log2", mathLog2, 1);
  defineMethod(mathObject, "log10", mathLog10, 1);
  defineMethod(mathObject, "exp", mathExp, 1);
  defineMethod(mathObject, "sin", mathSin, 1);
  defineMethod(mathObject, "cos", mathCos, 1);
  defineMethod(mathObject, "tan", mathTan, 1);
  defineMethod(mathObject, "asin", mathAsin, 1);
  defineMethod(mathObject, "acos", mathAcos, 1);
  defineMethod(mathObject, "atan", mathAtan, 1);
  defineMethod(mathObject, "atan2", mathAtan2, 2);
  defineMethod(mathObject, "hypot", mathHypot, -1);
  defineMethod(mathObject, "random", mathRandom, 0);
  csObjectSetProperty(mathObject, "PI", NUMBER_VAL(3.14159265358979323846));
  csObjectSetProperty(mathObject, "E", NUMBER_VAL(2.71828182845904523536));
  csObjectSetProperty(mathObject, "LN2", NUMBER_VAL(0.693147180559945309417));
  csObjectSetProperty(mathObject, "LN10", NUMBER_VAL(2.30258509299404568402));
  csObjectSetProperty(mathObject, "SQRT2", NUMBER_VAL(1.41421356237309504880));

  ObjObject *objectNamespace = defineNamespace("Object");
  defineMethod(objectNamespace, "keys", objectKeys, 1);
  defineMethod(objectNamespace, "values", objectValues, 1);
  defineMethod(objectNamespace, "entries", objectEntries, 1);
  defineMethod(objectNamespace, "assign", objectAssign, -1);
  defineMethod(objectNamespace, "hasOwn", objectHasOwn, 2);

  ObjObject *arrayNamespace = defineNamespace("Array");
  defineMethod(arrayNamespace, "isArray", arrayIsArray, 1);
  defineMethod(arrayNamespace, "of", arrayOf, -1);
  defineMethod(arrayNamespace, "from", arrayFrom, -1);

  /* Number is callable *and* a namespace, so it is defined as a function whose
   * statics carry the rest. */
  ObjNative *numberFn = csNativeNew(numberConvert, "Number", 1);
  csPushTempRoot((Obj *)numberFn);
  ObjObject *numberNamespace = csObjectNew("Number");
  csPushTempRoot((Obj *)numberNamespace);
  numberFn->statics = numberNamespace;
  defineGlobal("Number", OBJ_VAL(numberFn));
  defineMethod(numberNamespace, "isInteger", numberIsInteger, 1);
  defineMethod(numberNamespace, "isNaN", numberIsNaN, 1);
  defineMethod(numberNamespace, "isFinite", numberIsFinite, 1);
  defineMethod(numberNamespace, "parseInt", globalParseInt, -1);
  defineMethod(numberNamespace, "parseFloat", globalParseFloat, -1);
  csObjectSetProperty(numberNamespace, "MAX_SAFE_INTEGER", NUMBER_VAL(9007199254740991.0));
  csObjectSetProperty(numberNamespace, "MIN_SAFE_INTEGER", NUMBER_VAL(-9007199254740991.0));
  csObjectSetProperty(numberNamespace, "EPSILON", NUMBER_VAL(2.220446049250313e-16));
  csObjectSetProperty(numberNamespace, "MAX_VALUE", NUMBER_VAL(1.7976931348623157e308));
  csObjectSetProperty(numberNamespace, "MIN_VALUE", NUMBER_VAL(5e-324));

  ObjObject *jsonNamespace = defineNamespace("JSON");
  csJsonInstall(jsonNamespace);

  defineFunction("parseInt", globalParseInt, -1);
  defineFunction("parseFloat", globalParseFloat, -1);
  defineFunction("isNaN", globalIsNaN, -1);
  defineFunction("isFinite", globalIsFinite, -1);
  defineFunction("Error", errorConstruct, -1);
  defineFunction("AggregateError", aggregateErrorConstruct, -1);

  /* Explicit conversions, so nothing has to rely on implicit coercion. */
  csPopTempRoot();
  csPopTempRoot();

  defineFunction("String", stringConvert, 1);
  defineFunction("Boolean", booleanConvert, 1);

  csArrayMethodsInstall();
  csStringMethodsInstall();
  csPromiseMethodsInstall();
  csMapMethodsInstall();
  csGeneratorMethodsInstall();
  csRegexMethodsInstall();
  defineFunction("Map", csMapConstructorFn(), -1);
  defineFunction("Set", csSetConstructorFn(), -1);

  /* `Promise` is callable and also carries statics, the same shape `Number`
   * has: `new Promise(executor)` and `Promise.all([...])` both work. */
  ObjNative *promiseFn = csPromiseConstructor();
  csPushTempRoot((Obj *)promiseFn);
  ObjObject *promiseStatics = csObjectNew("Promise");
  csPushTempRoot((Obj *)promiseStatics);
  promiseFn->statics = promiseStatics;
  csPromiseInstallStatics(promiseStatics);
  csObjectFreeze(promiseStatics);
  defineGlobal("Promise", OBJ_VAL(promiseFn));
  csPopTempRoot();
  csPopTempRoot();

  defineFunction("setTimeout", csSetTimeoutFn(), -1);
  defineFunction("clearTimeout", csClearTimeoutFn(), -1);
  /* An interval is cancelled the same way a timeout is — the two share one
   * queue and one kind of handle, so one canceller is enough. */
  defineFunction("setInterval", csSetIntervalFn(), -1);
  defineFunction("clearInterval", csClearTimeoutFn(), -1);
  defineFunction("queueMicrotask", csQueueMicrotaskFn(), -1);

  defineGlobal("NaN", NUMBER_VAL(NAN));
  defineGlobal("Infinity", NUMBER_VAL(INFINITY));

  /* Sealed only now that every member is in place. From here the standard
   * library is read-only: `Math.PI = 3` and `console.log = f` are errors at
   * the line that writes them rather than mysteries somewhere later. */
  csObjectFreeze(console);
  csObjectFreeze(mathObject);
  csObjectFreeze(objectNamespace);
  csObjectFreeze(arrayNamespace);
  csObjectFreeze(numberNamespace);
  csObjectFreeze(jsonNamespace);
}
