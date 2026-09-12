/* native_descriptor.c — property descriptors.
 *
 * `Object.defineProperty` and `getOwnPropertyDescriptor`, which are the only
 * way from CScript to say what an accessor or a non-writable property is. Both
 * directions have to agree about the same four attributes, so they sit
 * together — and they are added to the `Object` namespace native_object.c
 * made rather than to one of their own.
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

/* ---------------- property descriptors ---------------- */

/* A descriptor says what a property is allowed to have done to it. Nothing
 * else in CScript needs the distinction — a property written the ordinary way
 * has all three attributes — so the machinery is entirely here and in the side
 * table an object grows only once one of these is used on it.
 *
 * `get`/`set` in a descriptor make an accessor, which is stored where the
 * accessors of an object literal's `get x()` already live: a class made for
 * that object alone. That is why an accessor never enters a shape and the
 * inline caches never see one. */
static bool readFlag(ObjObject *descriptor, const char *name, int length, bool *out) {
  Value flag;
  if (!csObjectGet(descriptor, csStringCopy(name, length), &flag)) return false;
  *out = csValueIsTruthy(flag);
  return true;
}

static bool readMember(ObjObject *descriptor, const char *name, int length, Value *out) {
  return csObjectGet(descriptor, csStringCopy(name, length), out);
}

/* True when `value` can be called — the only thing a `get` or `set` may be. */
static bool isCallable(Value value) {
  return IS_CLOSURE(value) || IS_NATIVE(value) || IS_BOUND_METHOD(value);
}

static bool defineOne(ObjObject *object, ObjString *key, Value describedBy) {
  if (!IS_OBJECT(describedBy)) {
    csVMRuntimeError("a property descriptor must be an object, got %s", csValueTypeName(describedBy));
    return false;
  }
  ObjObject *descriptor = AS_OBJECT(describedBy);

  if (object->frozen) {
    csVMRuntimeError("'%s' is frozen, so its properties cannot be redefined", object->name->chars);
    return false;
  }

  /* Redefining is refused when the existing property said it could not be.
   * A property that has never been described is configurable, so this only
   * ever stops what an earlier defineProperty asked to be stopped. */
  Value existing;
  bool present = csObjectGet(object, key, &existing);
  if (present && (csObjectAttributes(object, key) & CS_PROP_CONFIGURABLE) == 0) {
    csVMRuntimeError("'%s' is not configurable and cannot be redefined", key->chars);
    return false;
  }

  Value getter;
  Value setter;
  bool hasGetter = readMember(descriptor, "get", 3, &getter);
  bool hasSetter = readMember(descriptor, "set", 3, &setter);

  Value described;
  bool hasValue = readMember(descriptor, "value", 5, &described);
  if ((hasGetter || hasSetter) && hasValue) {
    csVMRuntimeError(
        "a property descriptor cannot have both a value and an "
        "accessor");
    return false;
  }

  /* An attribute a descriptor leaves out is false, not inherited: that is what
   * makes `Object.defineProperty(o, "x", { value: 1 })` produce a property
   * that is read-only and hidden, which surprises people and is nonetheless
   * exactly what JavaScript specifies. */
  bool writable = false;
  bool enumerable = false;
  bool configurable = false;
  readFlag(descriptor, "writable", 8, &writable);
  readFlag(descriptor, "enumerable", 10, &enumerable);
  readFlag(descriptor, "configurable", 12, &configurable);

  if (hasGetter || hasSetter) {
    if (hasGetter && !IS_UNDEFINED(getter) && !isCallable(getter)) {
      csVMRuntimeError("a descriptor's 'get' must be a function");
      return false;
    }
    if (hasSetter && !IS_UNDEFINED(setter) && !isCallable(setter)) {
      csVMRuntimeError("a descriptor's 'set' must be a function");
      return false;
    }
    if ((hasGetter && !IS_UNDEFINED(getter) && !IS_CLOSURE(getter)) || (hasSetter && !IS_UNDEFINED(setter) && !IS_CLOSURE(setter))) {
      /* The accessor tables hold closures, because that is what a class body
       * puts there and what the property paths call. */
      csVMRuntimeError(
          "a descriptor's accessor must be a function written in "
          "CScript");
      return false;
    }

    /* An accessor replaces any stored property of the same name: a name is
     * one or the other, never both. */
    if (present) csObjectDelete(object, key);

    if (object->klass == NULL) {
      object->klass = csClassNew(object->name);
      object->klass->isAccessorHolder = true;
    }
    if (hasGetter && !IS_UNDEFINED(getter)) {
      csTableSet(&object->klass->getters, key, getter);
    }
    if (hasSetter && !IS_UNDEFINED(setter)) {
      csTableSet(&object->klass->setters, key, setter);
    }
    /* The name takes a slot holding the stand-in, so that it is enumerated in
     * the order it was defined — the same thing a literal's `get x()` does. */
    csObjectLeaveShapeMode(object);
    csObjectPut(object, key, OBJ_VAL(vm.accessorMarker));
    /* An accessor is never writable in its own right — whether it can be
     * assigned to is decided by having a setter. */
    csObjectSetAttributes(object, key, (enumerable ? CS_PROP_ENUMERABLE : 0u) | (configurable ? CS_PROP_CONFIGURABLE : 0u));
    return true;
  }

  /* A data property. Leaving `value` out of a descriptor for a property that
   * already exists keeps the value it had. */
  if (!hasValue) described = present ? existing : UNDEFINED_VAL;

  /* Off the fast path first: the write path recognises a shape and stores
   * straight into the slot, so a read-only property must not be in one. */
  if (!writable) csObjectLeaveShapeMode(object);

  csObjectPut(object, key, described);
  csObjectSetAttributes(object, key, (writable ? CS_PROP_WRITABLE : 0u) | (enumerable ? CS_PROP_ENUMERABLE : 0u) | (configurable ? CS_PROP_CONFIGURABLE : 0u));
  return true;
}

static bool objectDefineProperty(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 3 || !IS_OBJECT(args[0])) {
    csVMRuntimeError(
        "Object.defineProperty expects an object, a key and a "
        "descriptor");
    return false;
  }
  if (!IS_STRING(args[1])) {
    csVMRuntimeError("Object.defineProperty expects a string key, got %s", csValueTypeName(args[1]));
    return false;
  }
  if (!defineOne(AS_OBJECT(args[0]), AS_STRING(args[1]), args[2])) return false;
  *result = args[0];
  return true;
}

/* Each own key of the second argument names a property to define. */
bool csNativeDefineFromMap(ObjObject *object, Value describedBy) {
  if (!IS_OBJECT(describedBy)) {
    csVMRuntimeError("expected an object of property descriptors, got %s", csValueTypeName(describedBy));
    return false;
  }
  ObjObject *map = AS_OBJECT(describedBy);

  for (int i = 0; i < csObjectCount(map); i++) {
    ObjString *key = csObjectKeyAt(map, i);
    if (!csObjectIsEnumerable(map, key)) continue;
    csPushTempRoot((Obj *)key);
    bool ok = defineOne(object, key, csObjectValueAt(map, i));
    csPopTempRoot();
    if (!ok) return false;
  }
  return true;
}

static bool objectDefineProperties(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 2 || !IS_OBJECT(args[0])) {
    csVMRuntimeError(
        "Object.defineProperties expects an object and a map of "
        "descriptors");
    return false;
  }
  if (!csNativeDefineFromMap(AS_OBJECT(args[0]), args[1])) return false;
  *result = args[0];
  return true;
}

/* The descriptor for one own property, or undefined when there is none.
 * Accessors report `get`/`set`; everything else reports `value`/`writable`. */
static bool describeOne(ObjObject *object, ObjString *key, Value *out) {
  unsigned attributes = csObjectAttributes(object, key);

  Value stored;
  bool present = csObjectGet(object, key, &stored);
  /* An own accessor holds the stand-in in its slot rather than a value. */
  bool isData = present && !csVMIsAccessorSlot(stored);

  Value getter = UNDEFINED_VAL;
  Value setter = UNDEFINED_VAL;
  if (!isData && object->klass != NULL) {
    ObjClosure *found = csClassFindGetter(object->klass, key);
    if (found != NULL) getter = OBJ_VAL(found);
    found = csClassFindSetter(object->klass, key);
    if (found != NULL) setter = OBJ_VAL(found);
  }
  if (!isData && IS_UNDEFINED(getter) && IS_UNDEFINED(setter)) {
    *out = UNDEFINED_VAL;
    return true;
  }

  ObjObject *descriptor = csObjectNew("Object");
  csPushTempRoot((Obj *)descriptor);
  if (isData) {
    csObjectSetProperty(descriptor, "value", stored);
    csObjectSetProperty(descriptor, "writable", BOOL_VAL((attributes & CS_PROP_WRITABLE) != 0));
  } else {
    csObjectSetProperty(descriptor, "get", getter);
    csObjectSetProperty(descriptor, "set", setter);
  }
  csObjectSetProperty(descriptor, "enumerable", BOOL_VAL((attributes & CS_PROP_ENUMERABLE) != 0));
  csObjectSetProperty(descriptor, "configurable", BOOL_VAL((attributes & CS_PROP_CONFIGURABLE) != 0));
  csPopTempRoot();

  *out = OBJ_VAL(descriptor);
  return true;
}

static bool objectGetOwnPropertyDescriptor(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 2 || !IS_OBJECT(args[0]) || !IS_STRING(args[1])) {
    csVMRuntimeError(
        "Object.getOwnPropertyDescriptor expects an object and a "
        "string");
    return false;
  }
  return describeOne(AS_OBJECT(args[0]), AS_STRING(args[1]), result);
}

static bool objectGetOwnPropertyDescriptors(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_OBJECT(args[0])) {
    csVMRuntimeError("Object.getOwnPropertyDescriptors expects an object");
    return false;
  }
  ObjObject *object = AS_OBJECT(args[0]);

  ObjObject *out = csObjectNew("Object");
  csPushTempRoot((Obj *)out);
  for (int i = 0; i < csObjectCount(object); i++) {
    ObjString *key = csObjectKeyAt(object, i);
    csPushTempRoot((Obj *)key);
    Value descriptor;
    bool ok = describeOne(object, key, &descriptor);
    if (ok) csObjectPut(out, key, descriptor);
    csPopTempRoot();
    if (!ok) {
      csPopTempRoot();
      return false;
    }
  }
  csPopTempRoot();

  *result = OBJ_VAL(out);
  return true;
}

void csNativeInstallDescriptors(ObjObject *objectNamespace) {
  csNativeDefineMethod(objectNamespace, "defineProperty", objectDefineProperty, 3);
  csNativeDefineMethod(objectNamespace, "defineProperties", objectDefineProperties, 2);
  csNativeDefineMethod(objectNamespace, "getOwnPropertyDescriptor", objectGetOwnPropertyDescriptor, 2);
  csNativeDefineMethod(objectNamespace, "getOwnPropertyDescriptors", objectGetOwnPropertyDescriptors, 1);
}
