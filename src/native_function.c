/* native_function.c — `call`, `apply` and `bind`.
 *
 * These matter less here than in JavaScript. A method already keeps its
 * receiver when it is called through a property, so the usual reason to write
 * `.bind(this)` does not arise. What is left is what they were named for:
 * calling something with an argument list you have rather than one you can
 * write, and fixing arguments in front of a function.
 *
 * A plain function has no `this` at all in CScript — slot 0 holds the callee
 * and nothing can name it — so a receiver given to one is accepted and
 * ignored. Only a function written as a method has somewhere to put it.
 */
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

static bool requireCallable(Value receiver, const char *method) {
  if (csValueIsCallable(receiver)) return true;
  csVMRuntimeError("'%s' needs a function, got %s", method,
                   csValueTypeName(receiver));
  return false;
}

/* Calls `target` with `receiver` in slot 0 and `args` after it. */
static bool invokeWith(Value target, Value receiver, Value *args, int argCount,
                       Value *result) {
  Value forwarded[UINT8_MAX];
  if (argCount > UINT8_MAX) {
    csVMRuntimeError("too many arguments (limit %d)", UINT8_MAX);
    return false;
  }
  for (int i = 0; i < argCount; i++) forwarded[i] = args[i];

  /* Going through a bound method is what puts the receiver in slot 0 — the
   * same path `obj.method` already takes, rather than a second way of doing
   * the same thing. */
  if (IS_CLOSURE(target) && AS_CLOSURE(target)->function->isMethod) {
    ObjBoundMethod *bound = csBoundMethodNew(receiver, AS_OBJ(target));
    csPushTempRoot((Obj *)bound);
    bool ok = csVMCallAdapted(OBJ_VAL(bound), forwarded, argCount, result);
    csPopTempRoot();
    return ok;
  }

  return csVMCallAdapted(target, forwarded, argCount, result);
}

static bool functionCall(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallable(receiver, "call")) return false;
  Value bindTo = argCount > 0 ? args[0] : UNDEFINED_VAL;
  return invokeWith(receiver, bindTo, args + (argCount > 0 ? 1 : 0),
                    argCount > 0 ? argCount - 1 : 0, result);
}

static bool functionApply(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallable(receiver, "apply")) return false;
  Value bindTo = argCount > 0 ? args[0] : UNDEFINED_VAL;

  if (argCount < 2 || IS_NULL(args[1]) || IS_UNDEFINED(args[1])) {
    return invokeWith(receiver, bindTo, NULL, 0, result);
  }
  if (!IS_ARRAY(args[1])) {
    csVMRuntimeError("'apply' expects an array of arguments, got %s",
                     csValueTypeName(args[1]));
    return false;
  }

  ObjArray *list = AS_ARRAY(args[1]);
  return invokeWith(receiver, bindTo, list->elements.values, list->elements.count,
                    result);
}

static bool functionBind(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireCallable(receiver, "bind")) return false;
  Value bindTo = argCount > 0 ? args[0] : UNDEFINED_VAL;

  ObjBoundMethod *bound = csBoundMethodNew(bindTo, AS_OBJ(receiver));
  csPushTempRoot((Obj *)bound);

  if (argCount > 1) {
    ObjArray *presets = csArrayNew();
    bound->presets = presets;
    for (int i = 1; i < argCount; i++) {
      csValueArrayWrite(&presets->elements, args[i]);
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(bound);
  return true;
}

static void defineFunctionMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.functionMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csFunctionMethodsInstall(void) {
  defineFunctionMethod("call", functionCall, -1);
  defineFunctionMethod("apply", functionApply, -1);
  defineFunctionMethod("bind", functionBind, -1);
}
