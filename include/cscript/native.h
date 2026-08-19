/* native.h — the built-in global environment (console, Math, and friends). */
#ifndef CSCRIPT_NATIVE_H
#define CSCRIPT_NATIVE_H

#include "cscript/common.h"

/* Installs every built-in into the VM's globals table. Call once, after
 * csVMInit and before interpreting anything. */
void csNativesInstall(void);

/* Registers the built-in array methods into vm.arrayMethods. Called by
 * csNativesInstall; separated only because there are a lot of them. */
void csArrayMethodsInstall(void);

#endif /* CSCRIPT_NATIVE_H */
