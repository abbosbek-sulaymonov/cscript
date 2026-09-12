/* native_process.c — `process`: where the program is running, and how to stop.
 *
 * Four things a script cannot work out for itself — its arguments, the
 * environment, the working directory, the platform — and one it cannot do:
 * exit with a status of its own choosing.
 *
 * `env` is a function rather than an object, which is the one place this
 * deliberately differs from Node. `process.env.HOME` reads a property that may
 * not be there, and in CScript reading an absent property is a runtime error
 * rather than `undefined` — so the Node spelling would turn every optional
 * variable into a crash. `process.env("HOME")` answers `undefined` and can be
 * given a `??` instead.
 *
 * This namespace is the one that is *not* frozen: `argv` is filled in after
 * the natives are installed, once the command line has been parsed. Nothing
 * else here can be reassigned, because every member is a method.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#include "native/native_internal.h"

/* Set by the platform that compiled this, because asking at runtime needs
 * uname and answers the kernel's name rather than the one people use. */
#if defined(__APPLE__)
#define CS_PLATFORM "darwin"
#elif defined(__linux__)
#define CS_PLATFORM "linux"
#elif defined(_WIN32)
#define CS_PLATFORM "win32"
#else
#define CS_PLATFORM "unknown"
#endif

extern char **environ;

static bool processEnv(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("process.env expects the name of a variable");
    return false;
  }

  const char *value = getenv(AS_STRING(args[0])->chars);
  if (value == NULL) {
    *result = UNDEFINED_VAL;
    return true;
  }
  *result = OBJ_VAL(csStringCopy(value, (int)strlen(value)));
  return true;
}

/* The names, so that a program can walk the environment it cannot read as an
 * object. Values are fetched one at a time through `env`, which keeps one copy
 * of the lookup rather than two. */
static bool processEnvNames(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  (void)argCount;
  (void)args;

  ObjArray *names = csArrayNew();
  csPushTempRoot((Obj *)names);
  for (char **entry = environ; entry != NULL && *entry != NULL; entry++) {
    const char *equals = strchr(*entry, '=');
    int length = equals != NULL ? (int)(equals - *entry) : (int)strlen(*entry);
    ObjString *name = csStringCopy(*entry, length);
    csNativeAppendRooted(names, OBJ_VAL(name));
  }
  *result = OBJ_VAL(names);
  csPopTempRoot();
  return true;
}

static bool processCwd(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  (void)argCount;
  (void)args;

  char path[4096];
  if (getcwd(path, sizeof path) == NULL) {
    csVMRuntimeError("the working directory could not be read");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(path, (int)strlen(path)));
  return true;
}

/* Ends the program *now*: no pending timers, no queued microtasks, no
 * collection. A program that wants its outstanding work finished should return
 * from the top level instead, which is what the event loop is for. */
static bool processExit(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  (void)result;

  int code = 0;
  if (argCount >= 1) {
    if (!IS_NUMBER(args[0])) {
      csVMRuntimeError("process.exit expects a number, or nothing");
      return false;
    }
    code = (int)AS_NUMBER(args[0]);
  }
  /* exit() flushes stdio, which matters: the last console.log before an exit
   * is usually the reason the exit is there. */
  exit(code);
}

void csNativeInstallProcess(void) {
  ObjObject *ns = csNativeDefineNamespace("process");

  /* Node's shape, because a program that reads it runs under Node too. It
   * starts empty and is filled by csVMSetScriptArgs once the command line has
   * been read — which is why this namespace is not frozen. */
  ObjArray *argv = csArrayNew();
  csPushTempRoot((Obj *)argv);
  csObjectSetProperty(ns, "argv", OBJ_VAL(argv));
  csPopTempRoot();

  csNativeDefineMethod(ns, "env", processEnv, 1);
  csNativeDefineMethod(ns, "envNames", processEnvNames, 0);
  csNativeDefineMethod(ns, "cwd", processCwd, 0);
  csNativeDefineMethod(ns, "exit", processExit, -1);

  csObjectSetProperty(ns, "platform", OBJ_VAL(csStringCopy(CS_PLATFORM, (int)strlen(CS_PLATFORM))));
  csObjectSetProperty(ns, "pid", NUMBER_VAL((double)getpid()));
}
