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

static bool requireGenerator(Value receiver, const char *method) {
  if (IS_GENERATOR(receiver)) return true;
  csVMRuntimeError("'%s' needs a generator, got %s", method,
                   csValueTypeName(receiver));
  return false;
}

static bool generatorNext(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireGenerator(receiver, "next")) return false;

  /* An async generator answers with a promise, because the body may await any
   * number of times before it reaches the `yield` that has the value. */
  if (AS_GENERATOR(receiver)->isAsync) {
    ObjPromise *pending = csGeneratorNextAsync(
        AS_GENERATOR(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL);
    *result = OBJ_VAL(pending);
    return true;
  }

  Value value;
  bool done;
  if (!csGeneratorNext(AS_GENERATOR(receiver),
                       argCount > 0 ? args[0] : UNDEFINED_VAL, &value, &done)) {
    return false;
  }
  *result = csIterationResult(value, done);
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

  Value record = csIterationResult(returned, true);
  if (!generator->isAsync) {
    *result = record;
    return true;
  }
  /* An async generator answers in a promise even when it has nothing left to
   * do, so `await it.return()` reads the same as `await it.next()`. */
  csPushTempRoot(AS_OBJ(record));
  ObjPromise *pending = csPromiseNew();
  csPromiseFulfill(pending, record);
  csPopTempRoot();
  *result = OBJ_VAL(pending);
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
