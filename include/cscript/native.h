/* native.h — the built-in global environment (console, Math, and friends). */
#ifndef CSCRIPT_NATIVE_H
#define CSCRIPT_NATIVE_H

#include "cscript/common.h"

/* Defined in object.h, which this header deliberately does not include. */
struct ObjObject;

/* Installs every built-in into the VM's globals table. Call once, after
 * csVMInit and before interpreting anything. */
void csNativesInstall(void);

/* Registers the built-in array methods into vm.arrayMethods. Called by
 * csNativesInstall; separated only because there are a lot of them. */
void csArrayMethodsInstall(void);

/* Registers the built-in string methods into vm.stringMethods. */
void csStringMethodsInstall(void);

/* Installs JSON.stringify and JSON.parse onto the given namespace object. */
void csJsonInstall(struct ObjObject *json);

#endif /* CSCRIPT_NATIVE_H */
