#include <stdio.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/table.h"
#include "cscript/vm.h"

/* FNV-1a. Cheap, well-distributed for short identifier-like keys. */
static uint32_t hashString(const char *key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619u;
  }
  return hash;
}

/* Links a freshly allocated object into the list the collector sweeps. */
static void registerObject(Obj *object, ObjType type) {
  object->type = type;
  object->isMarked = false;
  object->next = vm.objects;
  vm.objects = object;
}

static ObjString *allocateString(const char *chars, int length, uint32_t hash) {
  size_t size = sizeof(ObjString) + (size_t)length + 1;
  ObjString *string = (ObjString *)csReallocate(NULL, 0, size);
  registerObject((Obj *)string, OBJ_STRING);

  string->length = length;
  string->hash = hash;
  memcpy(string->chars, chars, (size_t)length);
  string->chars[length] = '\0';

  /* Interning can allocate, so keep the new string reachable across the insert. */
  csPushTempRoot((Obj *)string);
  csTableSet(&vm.strings, string, NULL_VAL);
  csPopTempRoot();

  return string;
}

ObjString *csStringCopy(const char *chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString *interned = csTableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) return interned;
  return allocateString(chars, length, hash);
}

ObjString *csStringTakeOwnership(char *chars, int length) {
  ObjString *result = csStringCopy(chars, length);
  csReallocate(chars, (size_t)length + 1, 0);
  return result;
}

ObjString *csStringConcat(ObjString *a, ObjString *b) {
  int length = a->length + b->length;
  char *chars = CS_ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, (size_t)a->length);
  memcpy(chars + a->length, b->chars, (size_t)b->length);
  chars[length] = '\0';
  return csStringTakeOwnership(chars, length);
}

ObjNative *csNativeNew(NativeFn function, const char *name, int arity) {
  /* Intern the name first: it allocates, and doing it after the ObjNative is
   * created would leave that object unreachable across a collection. */
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  ObjNative *native = CS_ALLOCATE(ObjNative, 1);
  registerObject((Obj *)native, OBJ_NATIVE);
  native->function = function;
  native->name = nameString;
  native->arity = arity;

  csPopTempRoot();
  return native;
}

ObjObject *csObjectNew(const char *name) {
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  ObjObject *object = CS_ALLOCATE(ObjObject, 1);
  registerObject((Obj *)object, OBJ_OBJECT);
  object->name = nameString;
  csTableInit(&object->properties);

  csPopTempRoot();
  return object;
}

void csObjectSetProperty(ObjObject *object, const char *name, Value value) {
  /* Both the interning and the table insert can allocate, so the receiver and
   * the value have to stay rooted for the whole operation. */
  csPushTempRoot((Obj *)object);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));

  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&object->properties, key, value);
  csPopTempRoot();

  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}

void csObjectPrint(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value));
      break;
    case OBJ_NATIVE:
      printf("[Function: %s]", AS_NATIVE(value)->name->chars);
      break;
    case OBJ_OBJECT:
      printf("[Object: %s]", AS_OBJECT(value)->name->chars);
      break;
  }
}

void csObjectBlacken(Obj *object) {
  switch (object->type) {
    case OBJ_STRING:
      break; /* no outgoing references */
    case OBJ_NATIVE:
      csMarkObject((Obj *)((ObjNative *)object)->name);
      break;
    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      csMarkObject((Obj *)instance->name);
      csTableMark(&instance->properties);
      break;
    }
  }
}

void csObjectFree(Obj *object) {
  switch (object->type) {
    case OBJ_STRING: {
      ObjString *string = (ObjString *)object;
      csReallocate(object, sizeof(ObjString) + (size_t)string->length + 1, 0);
      break;
    }
    case OBJ_NATIVE:
      CS_FREE(ObjNative, object);
      break;
    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      csTableFree(&instance->properties);
      CS_FREE(ObjObject, object);
      break;
    }
  }
}
