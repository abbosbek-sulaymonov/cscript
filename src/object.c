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
  object->keys = NULL;
  object->keyCount = 0;
  object->keyCapacity = 0;
  csTableInit(&object->properties);

  csPopTempRoot();
  return object;
}

void csObjectPut(ObjObject *object, ObjString *key, Value value) {
  bool isNewKey = csTableSet(&object->properties, key, value);
  if (!isNewKey) return;

  if (object->keyCapacity < object->keyCount + 1) {
    int oldCapacity = object->keyCapacity;
    object->keyCapacity = CS_GROW_CAPACITY(oldCapacity);
    object->keys =
        CS_GROW_ARRAY(ObjString *, object->keys, oldCapacity, object->keyCapacity);
  }
  object->keys[object->keyCount++] = key;
}

ObjFunction *csFunctionNew(void) {
  ObjFunction *function = CS_ALLOCATE(ObjFunction, 1);
  registerObject((Obj *)function, OBJ_FUNCTION);
  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  csChunkInit(&function->chunk);
  return function;
}

ObjUpvalue *csUpvalueNew(Value *slot) {
  ObjUpvalue *upvalue = CS_ALLOCATE(ObjUpvalue, 1);
  registerObject((Obj *)upvalue, OBJ_UPVALUE);
  upvalue->location = slot;
  upvalue->closed = NULL_VAL;
  upvalue->next = NULL;
  return upvalue;
}

ObjClosure *csClosureNew(ObjFunction *function) {
  /* Allocate the upvalue array first and clear it: if the closure allocation
   * triggers a collection, the collector must not walk uninitialised slots. */
  ObjUpvalue **upvalues = CS_ALLOCATE(ObjUpvalue *, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) upvalues[i] = NULL;

  ObjClosure *closure = CS_ALLOCATE(ObjClosure, 1);
  registerObject((Obj *)closure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
}

ObjArray *csArrayNew(void) {
  ObjArray *array = CS_ALLOCATE(ObjArray, 1);
  registerObject((Obj *)array, OBJ_ARRAY);
  csValueArrayInit(&array->elements);
  return array;
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

static void printFunctionName(const ObjFunction *function) {
  if (function->name == NULL) {
    printf("[Function: <script>]");
  } else {
    printf("[Function: %s]", function->name->chars);
  }
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
    case OBJ_FUNCTION:
      printFunctionName((ObjFunction *)AS_OBJ(value));
      break;
    case OBJ_CLOSURE:
      printFunctionName(AS_CLOSURE(value)->function);
      break;
    case OBJ_UPVALUE:
      /* Never reachable from user code; only the collector sees these. */
      printf("[upvalue]");
      break;

    case OBJ_ARRAY: {
      ObjArray *array = AS_ARRAY(value);
      printf("[ ");
      for (int i = 0; i < array->elements.count; i++) {
        if (i > 0) printf(", ");
        /* Strings are quoted inside a container, the way console.log does it,
         * so `[ "1" ]` and `[ 1 ]` are distinguishable. */
        if (IS_STRING(array->elements.values[i])) {
          printf("'%s'", AS_CSTRING(array->elements.values[i]));
        } else {
          csValuePrint(array->elements.values[i]);
        }
      }
      printf(" ]");
      break;
    }
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
      /* The key list holds the same strings the table does, but marking it too
       * keeps the two from disagreeing if the table ever drops an entry. */
      for (int i = 0; i < instance->keyCount; i++) {
        csMarkObject((Obj *)instance->keys[i]);
      }
      break;
    }

    case OBJ_FUNCTION: {
      /* A function owns its constant pool, so every literal in its body is
       * live for as long as the function is. */
      ObjFunction *function = (ObjFunction *)object;
      csMarkObject((Obj *)function->name);
      for (int i = 0; i < function->chunk.constants.count; i++) {
        csMarkValue(function->chunk.constants.values[i]);
      }
      break;
    }

    case OBJ_UPVALUE:
      /* `closed` holds the value once the variable has left the stack. While
       * the upvalue is still open it is empty, and the stack root covers it. */
      csMarkValue(((ObjUpvalue *)object)->closed);
      break;

    case OBJ_CLOSURE: {
      ObjClosure *closure = (ObjClosure *)object;
      csMarkObject((Obj *)closure->function);
      for (int i = 0; i < closure->upvalueCount; i++) {
        csMarkObject((Obj *)closure->upvalues[i]);
      }
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      for (int i = 0; i < array->elements.count; i++) {
        csMarkValue(array->elements.values[i]);
      }
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
      CS_FREE_ARRAY(ObjString *, instance->keys, instance->keyCapacity);
      CS_FREE(ObjObject, object);
      break;
    }

    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *)object;
      csChunkFree(&function->chunk);
      CS_FREE(ObjFunction, object);
      break;
    }

    case OBJ_UPVALUE:
      /* The captured value belongs to whoever else still references it. */
      CS_FREE(ObjUpvalue, object);
      break;

    case OBJ_CLOSURE: {
      /* The function is shared between closures, so only the array goes. */
      ObjClosure *closure = (ObjClosure *)object;
      CS_FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCount);
      CS_FREE(ObjClosure, object);
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      csValueArrayFree(&array->elements);
      CS_FREE(ObjArray, object);
      break;
    }
  }
}
