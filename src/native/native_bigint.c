/* native_bigint.c — whole numbers with no upper bound.
 *
 * A BigInt is written `123n` and is a type of its own, not a wider number.
 * That distinction is the point of it: `1n + 1` is refused rather than
 * answered, because either answer would be a lie about precision. Ordering
 * mixes freely, because `1n < 2` has exactly one right answer and the VM can
 * find it without rounding.
 *
 * The arithmetic itself is in bigint.c; this file is only the surface a script
 * sees — the `BigInt()` conversion and the two methods every value has.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* `BigInt(x)` — the one sanctioned way across the boundary.
 *
 * A number has to be whole: `BigInt(1.5)` throws in JavaScript rather than
 * truncating, for the same reason the operators refuse to mix. */
static bool bigintConvert(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("BigInt expects a value");
    return false;
  }

  Value from = args[0];
  if (IS_BIGINT(from)) {
    *result = from;
    return true;
  }

  BigInt parsed;
  csBigInit(&parsed);
  bool ok;
  if (IS_NUMBER(from)) {
    ok = csBigFromDouble(&parsed, AS_NUMBER(from));
    if (!ok) {
      csBigFree(&parsed);
      csVMRuntimeError("BigInt cannot convert %g — it is not a whole number",
                       AS_NUMBER(from));
      return false;
    }
  } else if (IS_BOOL(from)) {
    ok = csBigFromInt64(&parsed, AS_BOOL(from) ? 1 : 0);
  } else if (IS_STRING(from)) {
    ObjString *text = AS_STRING(from);
    /* An empty string is zero, which is what `Number("")` gives too. */
    const char *chars = text->chars;
    int length = text->length;
    while (length > 0 && (*chars == ' ' || *chars == '\t' || *chars == '\n')) {
      chars++;
      length--;
    }
    while (length > 0 && (chars[length - 1] == ' ' || chars[length - 1] == '\t' ||
                          chars[length - 1] == '\n')) {
      length--;
    }
    ok = length == 0 ? csBigFromInt64(&parsed, 0)
                     : csBigFromText(&parsed, chars, length);
    if (!ok) {
      csBigFree(&parsed);
      csVMRuntimeError("BigInt cannot convert '%s' — it is not a whole number",
                       text->chars);
      return false;
    }
  } else {
    csVMRuntimeError("BigInt cannot convert %s", csValueTypeName(from));
    return false;
  }

  if (!ok) {
    csBigFree(&parsed);
    csVMRuntimeError("out of memory converting to BigInt");
    return false;
  }

  *result = OBJ_VAL(csBigIntNew(parsed));
  return true;
}

/* The text of the number, in the requested base and without the `n`. The
 * suffix is how a BigInt is *written*, not what it says: `String(255n)` is
 * "255" in JavaScript, and only printing one shows the `n`. */
static bool bigintToString(Value receiver, int argCount, Value *args, Value *result) {
  int radix = 10;
  if (argCount > 0 && !IS_UNDEFINED(args[0])) {
    if (!IS_NUMBER(args[0])) {
      csVMRuntimeError("toString expects a number for the radix");
      return false;
    }
    radix = (int)AS_NUMBER(args[0]);
    if (radix < 2 || radix > 36) {
      csVMRuntimeError("toString radix must be between 2 and 36, got %d", radix);
      return false;
    }
  }

  char *text = csBigToText(&AS_BIGINT(receiver)->value, radix);
  if (text == NULL) {
    csVMRuntimeError("out of memory rendering a BigInt");
    return false;
  }

  *result = OBJ_VAL(csStringCopy(text, (int)strlen(text)));
  free(text);
  return true;
}

/* `valueOf` answers the BigInt itself, as it does on every primitive. */
static bool bigintValueOf(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  *result = receiver;
  return true;
}

static void defineBigIntMethod(const char *name, NativeFn function, int arity) {
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  csTableSet(&vm.bigintMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csBigIntMethodsInstall(void) {
  defineBigIntMethod("toString", bigintToString, -1);
  defineBigIntMethod("toLocaleString", bigintToString, -1);
  defineBigIntMethod("valueOf", bigintValueOf, 0);
}

NativeFn csBigIntConstructorFn(void) { return bigintConvert; }
