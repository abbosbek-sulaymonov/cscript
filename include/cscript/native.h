/* native.h — the built-in global environment (console, Math, and friends). */
#ifndef CSCRIPT_NATIVE_H
#define CSCRIPT_NATIVE_H

#include "cscript/object.h"

#include "cscript/common.h"

/* Defined in object.h, which this header deliberately does not include. */
struct ObjObject;

/* Installs every built-in into the VM's globals table. Call once, after
 * csVMInit and before interpreting anything. */
void csNativesInstall(void);

/* The value `Promise.any` rejects with, and what the `AggregateError` global
 * builds. `errors` becomes its `errors` property. */
Value csAggregateError(ObjArray *errors);
void csGeneratorMethodsInstall(void);
void csNumberMethodsInstall(void);

/* Registers the built-in array methods into vm.arrayMethods. Called by
 * csNativesInstall; separated only because there are a lot of them. */
void csArrayMethodsInstall(void);

/* Registers the built-in string methods into vm.stringMethods. */
void csStringMethodsInstall(void);

/* Installs JSON.stringify and JSON.parse onto the given namespace object. */
void csJsonInstall(struct ObjObject *json);

/* Promises, timers and the microtask queue. Split out of native.c because
 * they carry state of their own rather than being pure functions. */
void csPromiseMethodsInstall(void);
void csMapMethodsInstall(void);
void csRegexMethodsInstall(void);
bool csRegexStringSearch(Value receiver, int argCount, Value *args, Value *result);
bool csRegexStringMatch(Value receiver, int argCount, Value *args, Value *result);
bool csRegexStringReplace(Value receiver, int argCount, Value *args, Value *result,
                          bool all);
bool csRegexStringSplit(Value receiver, int argCount, Value *args, Value *result);
NativeFn csMapConstructorFn(void);
NativeFn csSetConstructorFn(void);
void csPromiseMarkRoots(void);
ObjNative *csPromiseConstructor(void);
void csPromiseInstallStatics(ObjObject *statics);
NativeFn csSetTimeoutFn(void);
NativeFn csSetIntervalFn(void);
NativeFn csClearTimeoutFn(void);
NativeFn csQueueMicrotaskFn(void);

#endif /* CSCRIPT_NATIVE_H */
