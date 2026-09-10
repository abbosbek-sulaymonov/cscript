/* native.c — the global environment, and what is installed into it.
 *
 * `console`, the error constructors, `Array`'s statics, and the one function
 * that builds all of it. Each namespace with a file of its own installs and
 * seals itself from there; what is left here is the wiring, and the two shapes
 * a built-in can have — a namespace, or something callable carrying statics.
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

/* `Array.from(source)` and `Array.from(source, fn)`.
 *
 * Anything it cannot convert is named rather than quietly answered with an
 * empty array, which is what this used to do — a wrong answer that looks like
 * a right one is the failure mode the whole project is written against. */
static bool arrayFrom(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("Array.from expects a value");
    return false;
  }

  Value mapper = argCount > 1 ? args[1] : UNDEFINED_VAL;
  bool hasMapper = !IS_UNDEFINED(mapper) && !IS_NULL(mapper);
  if (hasMapper && !IS_CLOSURE(mapper) && !IS_NATIVE(mapper) &&
      !IS_BOUND_METHOD(mapper)) {
    csVMRuntimeError("Array.from expects a function as its second argument, got %s",
                     csValueTypeName(mapper));
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
  } else if (IS_MAP(args[0]) && !AS_MAP(args[0])->isWeak) {
    /* A Set gives its members and a Map gives `[key, value]` pairs, which is
     * what csMapToArray already builds for spreading one. */
    ObjArray *entries = csMapToArray(AS_MAP(args[0]));
    /* Reachable from nothing else, and appending allocates. */
    csPushTempRoot((Obj *)entries);
    for (int i = 0; i < entries->elements.count; i++) {
      csValueArrayWrite(&array->elements, entries->elements.values[i]);
    }
    csPopTempRoot();
  } else if (IS_GENERATOR(args[0])) {
    /* Drained to its end, because the array has to be complete before it
     * exists. An endless generator does not terminate, which is equally true
     * of `Array.from` in JavaScript. */
    for (;;) {
      Value produced;
      bool done;
      if (!csGeneratorNext(AS_GENERATOR(args[0]), UNDEFINED_VAL, &produced, &done)) {
        csPopTempRoot();
        return false;
      }
      if (done) break;
      csValueArrayWrite(&array->elements, produced);
    }
  } else if (IS_OBJECT(args[0])) {
    /* An array-like: `{ length: 2 }` becomes two undefineds, and `{ 0: "a",
     * length: 1 }` becomes `["a"]`. */
    Value length;
    if (!csObjectGet(AS_OBJECT(args[0]), csStringCopy("length", 6), &length) ||
        !IS_NUMBER(length)) {
      csPopTempRoot();
      csVMRuntimeError("Array.from cannot convert an object without a numeric "
                       "'length'");
      return false;
    }
    int count = (int)AS_NUMBER(length);
    for (int i = 0; i < count; i++) {
      char digits[16];
      int written = snprintf(digits, sizeof digits, "%d", i);
      Value element;
      if (!csObjectGet(AS_OBJECT(args[0]), csStringCopy(digits, written), &element)) {
        element = UNDEFINED_VAL;
      }
      csValueArrayWrite(&array->elements, element);
    }
  } else {
    csPopTempRoot();
    csVMRuntimeError("Array.from cannot convert %s", csValueTypeName(args[0]));
    return false;
  }

  /* The mapping runs afterwards rather than as each element is taken, so a
   * callback that allocates cannot disturb a half-built source. */
  if (hasMapper) {
    for (int i = 0; i < array->elements.count; i++) {
      /* Adapted rather than called directly, so a one-parameter callback is
       * not handed the index it never asked for — the same courtesy `map` and
       * `filter` extend, and the reason CScript's strict arity is liveable. */
      Value callArgs[2] = {array->elements.values[i], NUMBER_VAL(i)};
      Value mapped;
      if (!csVMCallAdapted(mapper, callArgs, 2, &mapped)) {
        csPopTempRoot();
        return false;
      }
      array->elements.values[i] = mapped;
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(array);
  return true;
}

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
void csNativeDefineGlobal(const char *name, Value value) {
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

/* Builds a namespace object and installs it as a global.
 *
 * Namespaces are frozen at the end of csNativesInstall rather than here,
 * because they have no members yet. */
ObjObject *csNativeDefineNamespace(const char *name) {
  ObjObject *object = csObjectNew(name);
  csPushTempRoot((Obj *)object);
  csNativeDefineGlobal(name, OBJ_VAL(object));
  csPopTempRoot();
  return object;
}

void csNativeDefineMethod(ObjObject *object, const char *name, NativeFn function,
                         int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  csObjectSetProperty(object, name, OBJ_VAL(native));
  csPopTempRoot();
}

void csNativeDefineFunction(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  csNativeDefineGlobal(name, OBJ_VAL(native));
  csPopTempRoot();
}

/* A built-in that is both callable and a namespace: `Symbol(x)` and
 * `Symbol.iterator`, `new Date()` and `Date.now()`. The function carries the
 * statics rather than the two being separate globals, which is how JavaScript
 * has it and therefore how a program written against it expects it. */
static void installCallableNamespace(const char *name, NativeFn constructor,
                                     void (*installStatics)(ObjObject *)) {
  ObjNative *callable = csNativeNew(constructor, name, -1);
  csPushTempRoot((Obj *)callable);
  ObjObject *statics = csObjectNew(name);
  csPushTempRoot((Obj *)statics);
  callable->statics = statics;
  installStatics(statics);
  csObjectFreeze(statics);
  csNativeDefineGlobal(name, OBJ_VAL(callable));
  csPopTempRoot();
  csPopTempRoot();
}

/* The same shape, but the constructor comes ready-made rather than as a
 * plain NativeFn, because a promise's needs a closure over the VM's queue. */
static void installPromise(void) {
  ObjNative *promiseFn = csPromiseConstructor();
  csPushTempRoot((Obj *)promiseFn);
  ObjObject *statics = csObjectNew("Promise");
  csPushTempRoot((Obj *)statics);
  promiseFn->statics = statics;
  csPromiseInstallStatics(statics);
  csObjectFreeze(statics);
  csNativeDefineGlobal("Promise", OBJ_VAL(promiseFn));
  csPopTempRoot();
  csPopTempRoot();
}

void csNativesInstall(void) {
  /* Math.random is not cryptographic; one seed per process is enough. */
  srand((unsigned)time(NULL));

  ObjObject *console = csNativeDefineNamespace("console");
  csNativeDefineMethod(console, "log", consoleLog, -1);
  csNativeDefineMethod(console, "error", consoleError, -1);
  csNativeDefineMethod(console, "warn", consoleError, -1);
  csObjectFreeze(console);

  /* Each of these makes its namespace, fills it and seals it — see
   * native_internal.h for why the file that places the members is also the
   * file that freezes them. */
  csNativeInstallMath();
  csNativeInstallObject();
  csNativeInstallConversions();

  ObjObject *arrayNamespace = csNativeDefineNamespace("Array");
  csNativeDefineMethod(arrayNamespace, "isArray", arrayIsArray, 1);
  csNativeDefineMethod(arrayNamespace, "of", arrayOf, -1);
  csNativeDefineMethod(arrayNamespace, "from", arrayFrom, -1);
  csObjectFreeze(arrayNamespace);

  ObjObject *jsonNamespace = csNativeDefineNamespace("JSON");
  csJsonInstall(jsonNamespace);
  csObjectFreeze(jsonNamespace);

  csNativeDefineFunction("Error", errorConstruct, -1);
  csNativeDefineFunction("AggregateError", aggregateErrorConstruct, -1);
  csNativeDefineFunction("BigInt", csBigIntConstructorFn(), 1);

  csArrayMethodsInstall();
  csStringMethodsInstall();
  csPromiseMethodsInstall();
  csMapMethodsInstall();
  csGeneratorMethodsInstall();
  csNumberMethodsInstall();
  csFunctionMethodsInstall();
  csRegexMethodsInstall();
  csDateMethodsInstall();
  csWeakMethodsInstall();
  csSymbolMethodsInstall();
  csBigIntMethodsInstall();
  csNativeDefineFunction("Map", csMapConstructorFn(), -1);
  csNativeDefineFunction("Set", csSetConstructorFn(), -1);
  csNativeDefineFunction("WeakMap", csWeakMapConstructorFn(), -1);
  csNativeDefineFunction("WeakSet", csWeakSetConstructorFn(), -1);

  /* Three of the same shape: callable, and carrying statics beside it. `new
   * Date()` builds one and `Date.now()` sits next to it. */
  installCallableNamespace("Symbol", csSymbolConstructorFn(), csSymbolInstallStatics);
  installCallableNamespace("Date", csDateConstructorFn(), csDateInstallStatics);
  installPromise();

  csNativeDefineFunction("setTimeout", csSetTimeoutFn(), -1);
  csNativeDefineFunction("clearTimeout", csClearTimeoutFn(), -1);
  /* An interval is cancelled the same way a timeout is — the two share one
   * queue and one kind of handle, so one canceller is enough. */
  csNativeDefineFunction("setInterval", csSetIntervalFn(), -1);
  csNativeDefineFunction("clearInterval", csClearTimeoutFn(), -1);
  csNativeDefineFunction("queueMicrotask", csQueueMicrotaskFn(), -1);

  /* `process`, with the one member a command line needs. Deliberately not the
   * beginning of a Node-compatible surface: `argv` is here because a script
   * given arguments has to be able to read them, and it is spelled this way
   * because a program that reads it runs under Node too — which is the claim
   * the whole test suite is built to keep. */
  ObjObject *processObject = csNativeDefineNamespace("process");
  ObjArray *emptyArgs = csArrayNew();
  csPushTempRoot((Obj *)emptyArgs);
  csObjectSetProperty(processObject, "argv", OBJ_VAL(emptyArgs));
  csPopTempRoot();

  csNativeDefineGlobal("NaN", NUMBER_VAL(NAN));
  csNativeDefineGlobal("Infinity", NUMBER_VAL(INFINITY));
}
