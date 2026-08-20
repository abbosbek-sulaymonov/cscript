/* native_symbol.c — a name that is equal to nothing but itself.
 *
 * Two symbols with the same description are still two symbols: identity is the
 * whole of what one is. The description exists so that printing one says
 * something, and nothing else reads it.
 *
 * A symbol-keyed property is filed under a string no source can write, in the
 * table an object keeps beside its shape — the same place private fields live,
 * and for the same reason. That is what keeps such a property out of
 * `Object.keys`, out of JSON and out of a spread, which is where JavaScript
 * puts symbol keys too.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

static bool symbolConstruct(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;

  ObjString *description = NULL;
  if (argCount > 0 && !IS_UNDEFINED(args[0])) {
    if (IS_STRING(args[0])) {
      description = AS_STRING(args[0]);
    } else {
      size_t length = 0;
      char *text = csValueToCString(args[0], &length);
      if (text == NULL) {
        csVMRuntimeError("out of memory describing a symbol");
        return false;
      }
      description = csStringCopy(text, (int)length);
      free(text);
    }
  }

  *result = OBJ_VAL(csSymbolNew(description));
  return true;
}

/* `Symbol.for` is the one way two lookups can give the same symbol: it keeps a
 * registry keyed by the description, so separate parts of a program can agree
 * on a name without passing it between them. */
static bool symbolFor(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("Symbol.for expects a string");
    return false;
  }
  ObjString *key = AS_STRING(args[0]);

  Value existing;
  if (csTableGet(&vm.symbolRegistry, key, &existing)) {
    *result = existing;
    return true;
  }

  ObjSymbol *symbol = csSymbolNew(key);
  symbol->registered = true;
  csPushTempRoot((Obj *)symbol);
  csTableSet(&vm.symbolRegistry, key, OBJ_VAL(symbol));
  csPopTempRoot();

  *result = OBJ_VAL(symbol);
  return true;
}

static bool symbolKeyFor(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_SYMBOL(args[0])) {
    csVMRuntimeError("Symbol.keyFor expects a symbol");
    return false;
  }

  ObjSymbol *symbol = AS_SYMBOL(args[0]);
  *result = symbol->registered && symbol->description != NULL
                ? OBJ_VAL(symbol->description)
                : UNDEFINED_VAL;
  return true;
}

static bool symbolToString(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  if (!IS_SYMBOL(receiver)) {
    csVMRuntimeError("'toString' needs a symbol, got %s", csValueTypeName(receiver));
    return false;
  }

  size_t length = 0;
  char *text = csValueToCString(receiver, &length);
  if (text == NULL) {
    csVMRuntimeError("out of memory converting a symbol");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(text, (int)length));
  free(text);
  return true;
}

static void defineSymbolMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.symbolMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csSymbolMethodsInstall(void) {
  defineSymbolMethod("toString", symbolToString, 0);
}

NativeFn csSymbolConstructorFn(void) { return symbolConstruct; }

/* The well-known ones. They are ordinary symbols; what makes them well known
 * is that the language looks for them by name — `Symbol.iterator` is what
 * `for...of` asks an object for. */
static void defineWellKnown(ObjObject *statics, const char *name,
                            ObjSymbol **remember) {
  ObjString *described = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)described);
  ObjSymbol *symbol = csSymbolNew(described);
  csPushTempRoot((Obj *)symbol);

  csObjectSetProperty(statics, name, OBJ_VAL(symbol));
  *remember = symbol;

  csPopTempRoot();
  csPopTempRoot();
}

void csSymbolInstallStatics(ObjObject *statics) {
  ObjNative *forFn = csNativeNew(symbolFor, "for", 1);
  csPushTempRoot((Obj *)forFn);
  csObjectSetProperty(statics, "for", OBJ_VAL(forFn));
  csPopTempRoot();

  ObjNative *keyForFn = csNativeNew(symbolKeyFor, "keyFor", 1);
  csPushTempRoot((Obj *)keyForFn);
  csObjectSetProperty(statics, "keyFor", OBJ_VAL(keyForFn));
  csPopTempRoot();

  defineWellKnown(statics, "iterator", &vm.iteratorSymbol);
  defineWellKnown(statics, "asyncIterator", &vm.asyncIteratorSymbol);
}
