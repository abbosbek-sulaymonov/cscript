#include <stdio.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/shape.h"
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
  native->statics = NULL;

  csPopTempRoot();
  return native;
}

ObjObject *csObjectNew(const char *name) {
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  /* Nothing between the allocation and the last field write may allocate: the
   * object is on the sweep list from registerObject onward, so a collection
   * here would walk uninitialised slots. */
  ObjObject *object = CS_ALLOCATE(ObjObject, 1);
  registerObject((Obj *)object, OBJ_OBJECT);
  object->name = nameString;
  object->shape = vm.emptyShape;
  object->klass = NULL;
  object->frozen = false;
  object->as.slots.values = NULL;
  object->as.slots.capacity = 0;

  csPopTempRoot();
  return object;
}

static void ensureSlots(ObjObject *object, int needed) {
  if (object->as.slots.capacity >= needed) return;
  int oldCapacity = object->as.slots.capacity;
  int capacity = oldCapacity < 4 ? 4 : oldCapacity;
  while (capacity < needed) capacity *= 2;
  object->as.slots.values =
      CS_GROW_ARRAY(Value, object->as.slots.values, oldCapacity, capacity);
  object->as.slots.capacity = capacity;
}

/* Moves an object out of shape mode for good. Called once, when it grows past
 * the slot limit; see the comment on CS_SHAPE_MAX_SLOTS for why that limit
 * exists. Every allocation below happens while the object is still a valid
 * shape-mode object, so a collection in the middle is harmless. */
static void convertToDictionary(ObjObject *object) {
  Shape *shape = object->shape;
  int count = shape->slotCount;
  int capacity = count < 8 ? 8 : count;

  Table table;
  csTableInit(&table);
  ObjString **keys = CS_ALLOCATE(ObjString *, capacity);
  for (int i = 0; i < count; i++) {
    keys[i] = shape->keys[i];
    csTableSet(&table, shape->keys[i], object->as.slots.values[i]);
  }

  Value *oldValues = object->as.slots.values;
  int oldCapacity = object->as.slots.capacity;

  object->shape = NULL;
  object->as.dictionary.table = table;
  object->as.dictionary.keys = keys;
  object->as.dictionary.count = count;
  object->as.dictionary.capacity = capacity;

  CS_FREE_ARRAY(Value, oldValues, oldCapacity);
}

static void dictionaryPut(ObjObject *object, ObjString *key, Value value) {
  if (!csTableSet(&object->as.dictionary.table, key, value)) return;

  if (object->as.dictionary.capacity < object->as.dictionary.count + 1) {
    int oldCapacity = object->as.dictionary.capacity;
    object->as.dictionary.capacity = CS_GROW_CAPACITY(oldCapacity);
    object->as.dictionary.keys =
        CS_GROW_ARRAY(ObjString *, object->as.dictionary.keys, oldCapacity,
                      object->as.dictionary.capacity);
  }
  object->as.dictionary.keys[object->as.dictionary.count++] = key;
}

void csObjectPut(ObjObject *object, ObjString *key, Value value) {
  if (object->shape == NULL) {
    csPushTempRoot((Obj *)object);
    if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
    csPushTempRoot((Obj *)key);
    dictionaryPut(object, key, value);
    csPopTempRoot();
    if (IS_OBJ(value)) csPopTempRoot();
    csPopTempRoot();
    return;
  }

  /* Overwriting an existing property is the common case and never allocates,
   * so it is worth answering before any of the rooting below. */
  int slot;
  if (csShapeLookup(object->shape, key, &slot)) {
    object->as.slots.values[slot] = value;
    return;
  }

  csPushTempRoot((Obj *)object);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  csPushTempRoot((Obj *)key);

  Shape *next = csShapeTransition(object->shape, key);
  if (next == NULL) {
    convertToDictionary(object);
    dictionaryPut(object, key, value);
  } else {
    /* The new shape is reachable only through its parent's transition edge,
     * and that edge is weak — so until this object adopts it, a collection
     * would prune the edge and sweep the shape out from under us. Growing the
     * slot array is exactly such a collection point. */
    csPushTempRoot((Obj *)next);

    /* Order matters too. The collector sizes its walk of the slots from the
     * shape, so the object keeps its old shape until the new slot actually
     * holds a value. */
    ensureSlots(object, next->slotCount);
    object->as.slots.values[next->slotCount - 1] = value;
    object->shape = next;

    csPopTempRoot();
  }

  csPopTempRoot();
  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}

bool csObjectGet(ObjObject *object, ObjString *key, Value *out) {
  if (object->shape == NULL) {
    return csTableGet(&object->as.dictionary.table, key, out);
  }
  int slot;
  if (!csShapeLookup(object->shape, key, &slot)) return false;
  if (out != NULL) *out = object->as.slots.values[slot];
  return true;
}

int csObjectCount(const ObjObject *object) {
  return object->shape != NULL ? object->shape->slotCount
                               : object->as.dictionary.count;
}

ObjString *csObjectKeyAt(const ObjObject *object, int index) {
  return object->shape != NULL ? object->shape->keys[index]
                               : object->as.dictionary.keys[index];
}

Value csObjectValueAt(ObjObject *object, int index) {
  if (object->shape != NULL) return object->as.slots.values[index];
  Value value;
  return csTableGet(&object->as.dictionary.table,
                    object->as.dictionary.keys[index], &value)
             ? value
             : UNDEFINED_VAL;
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

ObjClass *csClassNew(ObjString *name) {
  csPushTempRoot((Obj *)name);
  ObjClass *klass = CS_ALLOCATE(ObjClass, 1);
  registerObject((Obj *)klass, OBJ_CLASS);
  klass->name = name;
  klass->superclass = NULL;
  klass->initializer = NULL;
  klass->fieldInit = NULL;
  csTableInit(&klass->methods);
  csTableInit(&klass->statics);
  csPopTempRoot();
  return klass;
}

ObjObject *csInstanceNew(ObjClass *klass) {
  csPushTempRoot((Obj *)klass);
  ObjObject *instance = CS_ALLOCATE(ObjObject, 1);
  registerObject((Obj *)instance, OBJ_OBJECT);
  instance->name = klass->name;
  instance->shape = vm.emptyShape;
  instance->klass = klass;
  instance->frozen = false;
  instance->as.slots.values = NULL;
  instance->as.slots.capacity = 0;
  csPopTempRoot();
  return instance;
}

ObjBoundMethod *csBoundMethodNew(Value receiver, ObjClosure *method) {
  if (IS_OBJ(receiver)) csPushTempRoot(AS_OBJ(receiver));
  csPushTempRoot((Obj *)method);
  ObjBoundMethod *bound = CS_ALLOCATE(ObjBoundMethod, 1);
  registerObject((Obj *)bound, OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  csPopTempRoot();
  if (IS_OBJ(receiver)) csPopTempRoot();
  return bound;
}

ObjClosure *csClassFindMethod(ObjClass *klass, ObjString *name) {
  for (ObjClass *current = klass; current != NULL; current = current->superclass) {
    Value method;
    if (csTableGet(&current->methods, name, &method)) {
      return (ObjClosure *)AS_OBJ(method);
    }
  }
  return NULL;
}

bool csClassDescendsFrom(const ObjClass *klass, const ObjClass *other) {
  for (const ObjClass *current = klass; current != NULL; current = current->superclass) {
    if (current == other) return true;
  }
  return false;
}

ObjArray *csArrayNew(void) {
  ObjArray *array = CS_ALLOCATE(ObjArray, 1);
  registerObject((Obj *)array, OBJ_ARRAY);
  csValueArrayInit(&array->elements);
  array->isSpreadMarker = false;
  return array;
}

void csObjectFreeze(ObjObject *object) { object->frozen = true; }

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
    case OBJ_CLASS: {
      ObjClass *klass = AS_CLASS(value);
      if (klass->superclass != NULL) {
        printf("[class %s extends %s]", klass->name->chars,
               klass->superclass->name->chars);
      } else {
        printf("[class %s]", klass->name->chars);
      }
      break;
    }
    case OBJ_BOUND_METHOD:
      printFunctionName(AS_BOUND_METHOD(value)->method->function);
      break;
    case OBJ_FUNCTION:
      printFunctionName((ObjFunction *)AS_OBJ(value));
      break;
    case OBJ_CLOSURE:
      printFunctionName(AS_CLOSURE(value)->function);
      break;
    case OBJ_UPVALUE:
    case OBJ_SHAPE:
      /* Never reachable from user code; only the collector sees these. */
      printf("[internal]");
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

    case OBJ_NATIVE: {
      ObjNative *native = (ObjNative *)object;
      csMarkObject((Obj *)native->name);
      csMarkObject((Obj *)native->statics);
      break;
    }

    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      csMarkObject((Obj *)instance->name);
      csMarkObject((Obj *)instance->klass);
      if (instance->shape != NULL) {
        /* The shape is the authority on how many slots hold a value. Anything
         * beyond slotCount is capacity the object has not grown into yet. */
        csMarkObject((Obj *)instance->shape);
        for (int i = 0; i < instance->shape->slotCount; i++) {
          csMarkValue(instance->as.slots.values[i]);
        }
      } else {
        csTableMark(&instance->as.dictionary.table);
        for (int i = 0; i < instance->as.dictionary.count; i++) {
          csMarkObject((Obj *)instance->as.dictionary.keys[i]);
        }
      }
      break;
    }

    case OBJ_SHAPE:
      csShapeBlacken((Shape *)object);
      break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csMarkObject((Obj *)klass->name);
      csMarkObject((Obj *)klass->superclass);
      csMarkObject((Obj *)klass->initializer);
      csMarkObject((Obj *)klass->fieldInit);
      csTableMark(&klass->methods);
      csTableMark(&klass->statics);
      break;
    }

    case OBJ_BOUND_METHOD: {
      ObjBoundMethod *bound = (ObjBoundMethod *)object;
      csMarkValue(bound->receiver);
      csMarkObject((Obj *)bound->method);
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
      if (instance->shape != NULL) {
        CS_FREE_ARRAY(Value, instance->as.slots.values, instance->as.slots.capacity);
      } else {
        csTableFree(&instance->as.dictionary.table);
        CS_FREE_ARRAY(ObjString *, instance->as.dictionary.keys,
                      instance->as.dictionary.capacity);
      }
      CS_FREE(ObjObject, object);
      break;
    }

    case OBJ_SHAPE:
      csShapeFree((Shape *)object);
      break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csTableFree(&klass->methods);
      csTableFree(&klass->statics);
      CS_FREE(ObjClass, object);
      break;
    }

    case OBJ_BOUND_METHOD:
      /* The receiver and the method both belong to whoever else holds them. */
      CS_FREE(ObjBoundMethod, object);
      break;

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
