/* native_generator.c — the methods a generator hands to whoever is pulling.
 *
 * A generator is a paused call. Everything below either resumes it, finishes
 * it, or reports what it produced — and the shape it reports in is the same
 * `{ value, done }` JavaScript's iterator protocol uses, so `for...of` and a
 * hand-written `while (!r.done)` see the same thing.
 */
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"
#include "vm_internal.h"

/* `{ value, done }`, the one shape every one of these returns. */
static Value iterationResult(Value value, bool done) {
  ObjObject *result = csObjectNew("Object");
  csPushTempRoot((Obj *)result);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));

  ObjString *valueKey = csStringCopy("value", 5);
  csPushTempRoot((Obj *)valueKey);
  csObjectPut(result, valueKey, value);
  csPopTempRoot();

  ObjString *doneKey = csStringCopy("done", 4);
  csPushTempRoot((Obj *)doneKey);
  csObjectPut(result, doneKey, BOOL_VAL(done));
  csPopTempRoot();

  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(result);
}

static bool requireGenerator(Value receiver, const char *method) {
  if (IS_GENERATOR(receiver)) return true;
  csVMRuntimeError("'%s' needs a generator, got %s", method,
                   csValueTypeName(receiver));
  return false;
}

static bool generatorNext(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireGenerator(receiver, "next")) return false;

  Value value;
  bool done;
  if (!csGeneratorNext(AS_GENERATOR(receiver),
                       argCount > 0 ? args[0] : UNDEFINED_VAL, &value, &done)) {
    return false;
  }
  *result = iterationResult(value, done);
  return true;
}

/* `.return(v)` abandons the body where it stands. The frame is simply never
 * resumed — there is nothing to unwind, because a suspended fiber owns its own
 * stack and dropping it drops everything on it. */
static bool generatorReturn(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireGenerator(receiver, "return")) return false;

  Value returned = argCount > 0 ? args[0] : UNDEFINED_VAL;
  ObjGenerator *generator = AS_GENERATOR(receiver);
  if (generator->running) {
    csVMRuntimeError("this generator is already running");
    return false;
  }

  csGeneratorFinish(generator, returned);
  generator->yielded = UNDEFINED_VAL;
  *result = iterationResult(returned, true);
  return true;
}

static void defineGeneratorMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.generatorMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csGeneratorMethodsInstall(void) {
  defineGeneratorMethod("next", generatorNext, -1);
  defineGeneratorMethod("return", generatorReturn, -1);
}
