/* object.c — the allocation every heap object goes through, and strings.
 *
 * Every constructor in the other object_*.c files ends up in registerObject,
 * which is what puts an object on the list the collector walks. Strings are
 * here because they are the one type with no constructor of its own to speak
 * of: they are interned, so making one is mostly finding out whether it
 * already exists.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/regex.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

#include "runtime/object_internal.h"

static uint32_t hashString(const char *key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619u;
  }
  return hash;
}

/* Links a freshly allocated object into the list the collector sweeps. */
void csObjectRegister(Obj *object, ObjType type) {
  object->type = type;
  object->isMarked = false;
  object->next = vm.objects;
  vm.objects = object;
}

static ObjString *allocateString(const char *chars, int length, uint32_t hash) {
  size_t size = sizeof(ObjString) + (size_t)length + 1;
  ObjString *string = (ObjString *)csReallocate(NULL, 0, size);
  csObjectRegister((Obj *)string, OBJ_STRING);

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
  csObjectRegister((Obj *)native, OBJ_NATIVE);
  native->function = function;
  native->name = nameString;
  native->arity = arity;
  native->statics = NULL;

  csPopTempRoot();
  return native;
}

void csObjectSetProperty(ObjObject *object, const char *name, Value value) {
  /* Both the interning and the table insert can allocate, so the receiver and
   * the value have to stay rooted for the whole operation. */
  csPushTempRoot((Obj *)object);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));

  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csObjectPut(object, key, value);
  csPopTempRoot();

  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}
