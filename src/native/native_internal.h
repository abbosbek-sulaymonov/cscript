/* native_internal.h — the seam between the standard library's files.
 *
 * The library is a namespace per file, and each one installs itself: it makes
 * its namespace object, fills it, and freezes it. Freezing is what makes the
 * built-ins constant — `Math.PI = 3` and `console.log = f` are errors at the
 * line that writes them rather than mysteries later — and it can only happen
 * once every member is in place, which is why the file that places them is
 * also the file that does it.
 *
 * Nothing outside src/native/ includes this.
 */
#ifndef CSCRIPT_NATIVE_INTERNAL_H
#define CSCRIPT_NATIVE_INTERNAL_H

#include "cscript/native.h"
#include "cscript/object.h"

/* Defines a global, keeping the value rooted across the table insert, and
 * marks it constant: a built-in is not something a program may reassign. */
void csNativeDefineGlobal(const char *name, Value value);

/* Builds a namespace object and installs it as a global. Not frozen here,
 * because it has no members yet. */
ObjObject *csNativeDefineNamespace(const char *name);

void csNativeDefineMethod(ObjObject *object, const char *name, NativeFn function,
                          int arity);
void csNativeDefineFunction(const char *name, NativeFn function, int arity);

/* Each namespace, installed and sealed by the file that owns it. */
void csNativeInstallMath(void);
void csNativeInstallObject(void);
void csNativeInstallConversions(void);

/* `Object`'s four descriptor methods, which live in native_descriptor.c
 * because the two directions have to agree about the same four attributes —
 * so they are added to the namespace native_object.c made rather than to one
 * of their own. */
void csNativeInstallDescriptors(ObjObject *objectNamespace);

/* An array receiver, spelled once. Every array method starts by taking one and
 * the cast is the same every time, so naming it keeps the intent visible. */
#define ARRAY_OF(receiver) (AS_ARRAY(receiver))

/* Appends with the value protected.
 *
 * Growing the element array allocates, which can collect. A value that only
 * exists in a C local at that moment — a callback's return value, say — is
 * reachable from nothing the collector scans, so it has to be rooted across
 * the write. `make test-gc` found this the hard way. */
void csNativeAppendRooted(ObjArray *array, Value value);

/* The three a string method cannot do without: reading a string argument with
 * the error message the method should give, handing a freshly built buffer to
 * an interned ObjString, and finding a substring from an offset. Shared
 * because the pattern methods need all three and live in a file of their own.
 * */
bool csNativeStringArg(int argCount, Value *args, int position, const char *method,
                       ObjString **out);
bool csNativeFinishString(char *buffer, int length, Value *result);
int csNativeFindFrom(ObjString *haystack, ObjString *needle, int from);

/* A method on every array, or on every string. Both live in a table of their
 * own rather than on a prototype object, because an array and a string are not
 * property bags — so `[].map` is a lookup in vm.arrayMethods, not a walk up a
 * chain that does not exist. */
void csNativeDefineArrayMethod(const char *name, NativeFn function, int arity);
void csNativeDefineStringMethod(const char *name, NativeFn function, int arity);

/* The halves of each of those two groups that live in a file of their own. */
void csNativeInstallArrayCallbacks(void);
void csNativeInstallStringSearch(void);

/* Applies a `{ key: descriptor, ... }` map to an object, which is what both
 * `Object.defineProperties` and `Object.create`'s second argument are — so it
 * is shared rather than written twice. */
bool csNativeDefineFromMap(ObjObject *object, Value describedBy);

#endif /* CSCRIPT_NATIVE_INTERNAL_H */
