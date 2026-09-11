/* native_object.c — the `Object` namespace.
 *
 * Reading an object's own properties in the order it holds them, which is
 * exactly why ObjObject keeps that list alongside its hash table. Everything
 * here works on a property bag and says so rather than coercing.
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

/* ---------------- Object ---------------- */

/* Walks an object's keys in insertion order, which is exactly why ObjObject
 * keeps that list alongside its hash table. */
static bool objectEnumerate(int argCount, Value *args, Value *result, int mode,
                            const char *method) {
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.%s expects an object, got %s", method,
                     argCount >= 1 ? csValueTypeName(args[0]) : "no argument");
    return false;
  }
  ObjObject *object = AS_OBJECT(args[0]);

  ObjArray *out = csArrayNew();
  csPushTempRoot((Obj *)out);

  for (int i = 0; i < csObjectCount(object); i++) {
    ObjString *key = csObjectKeyAt(object, i);
    if (!csObjectIsEnumerable(object, key)) continue;
    /* An accessor's value is what its getter answers, which is a call. */
    Value value;
    if (!csVMReadOwnProperty(object, key, &value)) {
      csPopTempRoot();
      return false;
    }

    if (mode == 0) {
      csValueArrayWrite(&out->elements, OBJ_VAL(key));
    } else if (mode == 1) {
      if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
      csValueArrayWrite(&out->elements, value);
      if (IS_OBJ(value)) csPopTempRoot();
    } else {
      /* entries: a two-element array per property. */
      ObjArray *pair = csArrayNew();
      csPushTempRoot((Obj *)pair);
      csValueArrayWrite(&pair->elements, OBJ_VAL(key));
      csValueArrayWrite(&pair->elements, value);
      csValueArrayWrite(&out->elements, OBJ_VAL(pair));
      csPopTempRoot();
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(out);
  return true;
}

static bool objectKeys(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 0, "keys");
}
static bool objectValues(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 1, "values");
}
static bool objectEntries(Value r, int c, Value *a, Value *out) {
  (void)r;
  return objectEnumerate(c, a, out, 2, "entries");
}

/* `Object.fromEntries` — the inverse of `Object.entries`, and the reason a
 * Map and an object can be converted into one another at all. */
static bool objectFromEntries(Value receiver, int argCount, Value *args,
                              Value *result) {
  (void)receiver;
  Value source = argCount > 0 ? args[0] : UNDEFINED_VAL;
  if (IS_MAP(source)) source = OBJ_VAL(csMapToArray(AS_MAP(source)));
  if (!IS_ARRAY(source)) {
    csVMRuntimeError("Object.fromEntries expects an array of pairs or a Map");
    return false;
  }

  ObjArray *pairs = AS_ARRAY(source);
  csPushTempRoot((Obj *)pairs);
  ObjObject *built = csObjectNew("Object");
  csPushTempRoot((Obj *)built);

  for (int i = 0; i < pairs->elements.count; i++) {
    Value pair = pairs->elements.values[i];
    if (!IS_ARRAY(pair) || AS_ARRAY(pair)->elements.count < 2) {
      csPopTempRoot();
      csPopTempRoot();
      csVMRuntimeError("Object.fromEntries expects each entry to be a pair");
      return false;
    }
    Value key = AS_ARRAY(pair)->elements.values[0];
    if (!IS_STRING(key)) {
      size_t length = 0;
      char *text = csValueToCString(key, &length);
      if (text == NULL) {
        csPopTempRoot();
        csPopTempRoot();
        csVMRuntimeError("out of memory building an object key");
        return false;
      }
      ObjString *converted = csStringCopy(text, (int)length);
      free(text);
      csPushTempRoot((Obj *)converted);
      csObjectPut(built, converted, AS_ARRAY(pair)->elements.values[1]);
      csPopTempRoot();
      continue;
    }
    csObjectPut(built, AS_STRING(key), AS_ARRAY(pair)->elements.values[1]);
  }

  csPopTempRoot();
  csPopTempRoot();
  *result = OBJ_VAL(built);
  return true;
}

/* `Object.freeze` is what the standard library already does to itself: a
 * frozen object refuses every write, rather than ignoring it silently the way
 * non-strict JavaScript does. */
static bool objectFreeze(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("Object.freeze expects an object");
    return false;
  }
  if (IS_OBJECT(args[0])) AS_OBJECT(args[0])->frozen = true;
  *result = args[0];
  return true;
}

static bool objectIsFrozen(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  *result = BOOL_VAL(argCount > 0 && IS_OBJECT(args[0]) && AS_OBJECT(args[0])->frozen);
  return true;
}

/* The symbol keys an object carries. They live beside the shape, so they are
 * not among what `Object.keys` reports — this is the one way to ask for
 * them, which is also true in JavaScript. */
static bool objectGetOwnPropertySymbols(Value receiver, int argCount, Value *args,
                                        Value *result) {
  (void)receiver;
  ObjArray *found = csArrayNew();
  csPushTempRoot((Obj *)found);

  if (argCount > 0 && IS_OBJECT(args[0])) {
    csVMCollectSymbolKeys(AS_OBJECT(args[0]), found);
  }

  csPopTempRoot();
  *result = OBJ_VAL(found);
  return true;
}

static bool objectAssign(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.assign expects a target object");
    return false;
  }
  ObjObject *target = AS_OBJECT(args[0]);

  for (int i = 1; i < argCount; i++) {
    if (!IS_OBJECT(args[i])) continue;
    ObjObject *source = AS_OBJECT(args[i]);
    for (int j = 0; j < csObjectCount(source); j++) {
      ObjString *key = csObjectKeyAt(source, j);
      if (!csObjectIsEnumerable(source, key)) continue;
      Value value;
      if (!csVMReadOwnProperty(source, key, &value)) return false;
      csObjectPut(target, key, value);
    }
  }

  *result = args[0];
  return true;
}

static bool objectHasOwn(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 2 || !IS_OBJECT(args[0]) || !IS_STRING(args[1])) {
    csVMRuntimeError("Object.hasOwn expects an object and a string");
    return false;
  }
  *result = BOOL_VAL(csObjectGet(AS_OBJECT(args[0]), AS_STRING(args[1]), NULL));
  return true;
}

/* Defined below, with the rest of the descriptor machinery. */
bool csNativeDefineFromMap(ObjObject *object, Value describedBy);

/* `Object.create(proto)` — a new object that inherits from `proto` and owns
 * nothing. This is the prototype model without a constructor function: the
 * shared behaviour is an ordinary object, and what inherits from it is made
 * here rather than by `new`. */
static bool objectCreate(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || (!IS_OBJECT(args[0]) && !IS_NULL(args[0]))) {
    csVMRuntimeError("Object.create expects an object or null");
    return false;
  }
  ObjObject *created = csObjectNew("Object");
  if (IS_OBJECT(args[0])) created->prototype = AS_OBJECT(args[0]);

  if (argCount > 1 && !IS_UNDEFINED(args[1]) && !IS_NULL(args[1])) {
    csPushTempRoot((Obj *)created);
    bool ok = csNativeDefineFromMap(created, args[1]);
    csPopTempRoot();
    if (!ok) return false;
  }

  *result = OBJ_VAL(created);
  return true;
}

static bool objectGetPrototypeOf(Value receiver, int argCount, Value *args,
                                 Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.getPrototypeOf expects an object");
    return false;
  }
  ObjObject *prototype = AS_OBJECT(args[0])->prototype;
  *result = prototype != NULL ? OBJ_VAL(prototype) : NULL_VAL;
  return true;
}

static bool objectSetPrototypeOf(Value receiver, int argCount, Value *args,
                                 Value *result) {
  (void)receiver;
  if (argCount < 2 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.setPrototypeOf expects an object and a prototype");
    return false;
  }
  ObjObject *object = AS_OBJECT(args[0]);

  if (IS_NULL(args[1]) || IS_UNDEFINED(args[1])) {
    object->prototype = NULL;
    *result = args[0];
    return true;
  }
  if (!IS_OBJECT(args[1])) {
    csVMRuntimeError("a prototype must be an object or null, got %s",
                     csValueTypeName(args[1]));
    return false;
  }
  if (!csObjectSetPrototype(object, AS_OBJECT(args[1]))) {
    csVMRuntimeError("that prototype is already in this object\'s chain, which "
                     "would make every lookup on it loop");
    return false;
  }
  *result = args[0];
  return true;
}

/* What kind of thing this is, to a program that has to ask.
 *
 * `typeof` answers "object" for an object, an array, a Map, a Set, a Date and
 * a class instance alike, and nothing else in the language can separate them:
 * every Object method that would tell you — keys, entries, getPrototypeOf,
 * hasOwn — *refuses* a Map rather than answering about it. So a library that
 * walks a value structurally had no safe way to ask what it was walking, and
 * a deep copy or a deep comparison would take the process down on a Map
 * rather than mis-handle it.
 *
 * This answers for anything, and never fails. The names are the ones a program
 * would write in a comparison, which is why an array is "array" rather than
 * "object" and null is "null" rather than a kind of nothing. */
static bool objectKindOf(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("Object.kindOf expects a value");
    return false;
  }

  Value value = args[0];
  const char *kind = NULL;
  if (IS_ARRAY(value)) {
    kind = "array";
  } else if (IS_MAP(value)) {
    /* One structure with two flags underneath, and four names above it —
     * which is the distinction a caller is asking about. */
    ObjMap *map = AS_MAP(value);
    if (map->isWeak) {
      kind = map->isSet ? "weakset" : "weakmap";
    } else {
      kind = map->isSet ? "set" : "map";
    }
  } else if (IS_DATE(value)) {
    kind = "date";
  } else if (IS_REGEX(value)) {
    kind = "regex";
  } else if (IS_PROMISE(value)) {
    kind = "promise";
  } else if (IS_GENERATOR(value)) {
    kind = "generator";
  } else if (IS_CLASS(value)) {
    kind = "class";
  } else if (IS_OBJECT(value)) {
    /* An instance of a user class knows its class; a plain object does not,
     * and the difference is what a structural walk needs. */
    kind = AS_OBJECT(value)->klass != NULL ? "instance" : "object";
  } else {
    /* Everything else is what typeof already says, and agrees with it. */
    kind = csValueTypeName(value);
  }

  *result = OBJ_VAL(csStringCopy(kind, (int)strlen(kind)));
  return true;
}

void csNativeInstallObject(void) {
  ObjObject *ns = csNativeDefineNamespace("Object");
  csNativeDefineMethod(ns, "keys", objectKeys, 1);
  csNativeDefineMethod(ns, "values", objectValues, 1);
  csNativeDefineMethod(ns, "entries", objectEntries, 1);
  csNativeDefineMethod(ns, "assign", objectAssign, -1);
  csNativeDefineMethod(ns, "hasOwn", objectHasOwn, 2);
  csNativeDefineMethod(ns, "create", objectCreate, -1);
  csNativeDefineMethod(ns, "kindOf", objectKindOf, 1);
  csNativeDefineMethod(ns, "getPrototypeOf", objectGetPrototypeOf, 1);
  csNativeDefineMethod(ns, "setPrototypeOf", objectSetPrototypeOf, 2);
  csNativeDefineMethod(ns, "fromEntries", objectFromEntries, 1);
  csNativeDefineMethod(ns, "freeze", objectFreeze, 1);
  csNativeDefineMethod(ns, "isFrozen", objectIsFrozen, 1);
  /* Every own key, which for an object with no non-enumerable ones is the
   * same list `keys` gives — and here there are none. */
  csNativeDefineMethod(ns, "getOwnPropertyNames", objectKeys, 1);
  csNativeDefineMethod(ns, "getOwnPropertySymbols", objectGetOwnPropertySymbols, 1);

  csNativeInstallDescriptors(ns);
  csObjectFreeze(ns);
}
