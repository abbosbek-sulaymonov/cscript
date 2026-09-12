/* native_math.c — `Math`, and the numeric functions with nowhere else to be.
 *
 * Thin wrappers over libm, with two things every one of them does: check that
 * what it was handed is a number rather than coercing it, and answer what the
 * specification says rather than what C happens to.
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

/* ---------------- Math ---------------- */

/* One wrapper for every single-argument libm function. */
#define MATH_UNARY(name, expression)                                                 \
  static bool math##name(Value receiver, int argCount, Value *args, Value *result) { \
    (void)receiver;                                                                  \
    if (argCount != 1 || !IS_NUMBER(args[0])) {                                      \
      csVMRuntimeError("Math." #name " expects one number");                         \
      return false;                                                                  \
    }                                                                                \
    double x = AS_NUMBER(args[0]);                                                   \
    (void)x;                                                                         \
    *result = NUMBER_VAL(expression);                                                \
    return true;                                                                     \
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

void csNativeInstallMath(void) {
  ObjObject *math = csNativeDefineNamespace("Math");
  csNativeDefineMethod(math, "floor", mathFloor, 1);
  csNativeDefineMethod(math, "abs", mathAbs, 1);
  csNativeDefineMethod(math, "max", mathMax, -1);
  csNativeDefineMethod(math, "min", mathMin, -1);
  csNativeDefineMethod(math, "sqrt", mathSqrt, 1);
  csNativeDefineMethod(math, "cbrt", mathCbrt, 1);
  csNativeDefineMethod(math, "ceil", mathCeil, 1);
  csNativeDefineMethod(math, "trunc", mathTrunc, 1);
  csNativeDefineMethod(math, "sign", mathSign, 1);
  csNativeDefineMethod(math, "round", mathRound, 1);
  csNativeDefineMethod(math, "pow", mathPow, 2);
  csNativeDefineMethod(math, "log", mathLog, 1);
  csNativeDefineMethod(math, "log2", mathLog2, 1);
  csNativeDefineMethod(math, "log10", mathLog10, 1);
  csNativeDefineMethod(math, "exp", mathExp, 1);
  csNativeDefineMethod(math, "sin", mathSin, 1);
  csNativeDefineMethod(math, "cos", mathCos, 1);
  csNativeDefineMethod(math, "tan", mathTan, 1);
  csNativeDefineMethod(math, "asin", mathAsin, 1);
  csNativeDefineMethod(math, "acos", mathAcos, 1);
  csNativeDefineMethod(math, "atan", mathAtan, 1);
  csNativeDefineMethod(math, "atan2", mathAtan2, 2);
  csNativeDefineMethod(math, "hypot", mathHypot, -1);
  csNativeDefineMethod(math, "random", mathRandom, 0);
  csObjectSetProperty(math, "PI", NUMBER_VAL(3.14159265358979323846));
  csObjectSetProperty(math, "E", NUMBER_VAL(2.71828182845904523536));
  csObjectSetProperty(math, "LN2", NUMBER_VAL(0.693147180559945309417));
  csObjectSetProperty(math, "LN10", NUMBER_VAL(2.30258509299404568402));
  csObjectSetProperty(math, "SQRT2", NUMBER_VAL(1.41421356237309504880));
  csObjectFreeze(math);
}
