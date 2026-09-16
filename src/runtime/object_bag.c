/* object_bag.c — an object's properties: slots while the layout is shared,
 * a table once it is not.
 *
 * A property bag starts in shape mode, where the layout is a Shape shared with
 * every other object that reached it the same way and the values are a flat
 * array indexed by it. Deleting a property, or adding more than a shape chain
 * is worth, tips it into dictionary mode — after which it carries its own
 * table and every inline cache that named it misses for good.
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

ObjObject *csObjectNew(const char *name) {
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  /* Nothing between the allocation and the last field write may allocate: the
   * object is on the sweep list from registerObject onward, so a collection
   * here would walk uninitialised slots. */
  ObjObject *object = CS_ALLOCATE(ObjObject, 1);
  csObjectRegister((Obj *)object, OBJ_OBJECT);
  object->name = nameString;
  object->shape = vm.emptyShape;
  object->klass = NULL;
  object->frozen = false;
  object->builtByConstructor = false;
  object->prototype = NULL;
  object->privates = NULL;
  object->attributes = NULL;
  object->as.slots.values = NULL;
  object->as.slots.capacity = 0;

  csPopTempRoot();
  return object;
}

static void ensureSlots(ObjObject *object, int needed);

void csObjectReserveSlots(ObjObject *object, int slots) {
  if (object->shape == NULL) return;
  ensureSlots(object, slots);
}

static void ensureSlots(ObjObject *object, int needed) {
  if (object->as.slots.capacity >= needed) return;
  int oldCapacity = object->as.slots.capacity;
  int capacity = oldCapacity < 4 ? 4 : oldCapacity;
  while (capacity < needed) capacity *= 2;
  object->as.slots.values = CS_GROW_ARRAY(Value, object->as.slots.values, oldCapacity, capacity);
  object->as.slots.capacity = capacity;
}

/* Moves an object out of shape mode for good. Called when it grows past the
 * slot limit — see the comment on CS_SHAPE_MAX_SLOTS for why that limit
 * exists — and when `Object.defineProperty` makes one of its properties
 * non-writable, because the write fast path recognises a shape and stores
 * straight into the slot without asking anything else. Leaving shape mode is
 * how such a property stops being reachable that way, and it costs only the
 * object it happened to, rather than a test on every write in the program.
 *
 * Every allocation below happens while the object is still a valid shape-mode
 * object, so a collection in the middle is harmless. */
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
    object->as.dictionary.keys = CS_GROW_ARRAY(ObjString *, object->as.dictionary.keys, oldCapacity, object->as.dictionary.capacity);
  }
  object->as.dictionary.keys[object->as.dictionary.count++] = key;
}

bool csObjectGetPrivate(ObjObject *object, ObjString *key, Value *out) {
  if (object->privates == NULL) return false;
  return csTableGet(object->privates, key, out);
}

bool csObjectDeletePrivate(ObjObject *object, ObjString *key) {
  if (object->privates == NULL) return false;
  return csTableDelete(object->privates, key);
}

void csObjectPutPrivate(ObjObject *object, ObjString *key, Value value) {
  if (object->privates == NULL) {
    csPushTempRoot((Obj *)object);
    csPushTempRoot((Obj *)key);
    if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
    Table *table = CS_ALLOCATE(Table, 1);
    csTableInit(table);
    object->privates = table;
    if (IS_OBJ(value)) csPopTempRoot();
    csPopTempRoot();
    csPopTempRoot();
  }
  csTableSet(object->privates, key, value);
}

bool csObjectDelete(ObjObject *object, ObjString *key) {
  if (object->shape != NULL) {
    int slot;
    if (!csShapeLookup(object->shape, key, &slot)) return false;

    /* A shape names a fixed set of slots in a fixed order, and what is left
     * after removing one describes nothing in the transition tree. So the
     * object leaves shape mode for good — the same escape hatch growing past
     * the slot limit takes. That is the real cost of `delete`, and the reason
     * to reach for it rarely rather than the reason to refuse it. */
    csPushTempRoot((Obj *)object);
    csPushTempRoot((Obj *)key);
    convertToDictionary(object);
    csPopTempRoot();
    csPopTempRoot();
  }

  if (!csTableDelete(&object->as.dictionary.table, key)) return false;

  /* Insertion order is kept as a list, so the key leaves that too. */
  for (int i = 0; i < object->as.dictionary.count; i++) {
    if (object->as.dictionary.keys[i] != key) continue;
    for (int j = i; j + 1 < object->as.dictionary.count; j++) {
      object->as.dictionary.keys[j] = object->as.dictionary.keys[j + 1];
    }
    object->as.dictionary.count--;
    break;
  }
  return true;
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

/* Own properties first, then the chain. The loop is bounded by the cycle check
 * in csObjectSetPrototype, which is the only way a chain is ever built. */
bool csObjectGetInherited(ObjObject *object, ObjString *key, Value *out) {
  for (ObjObject *at = object; at != NULL; at = at->prototype) {
    if (csObjectGet(at, key, out)) return true;
  }
  return false;
}

bool csObjectHasInherited(ObjObject *object, ObjString *key) {
  return csObjectGetInherited(object, key, NULL);
}

bool csObjectSetPrototype(ObjObject *object, ObjObject *prototype) {
  /* A chain that reaches back to the object would make every miss loop
   * forever, so the check is what makes the walk above safe to write as a
   * plain loop. */
  for (ObjObject *at = prototype; at != NULL; at = at->prototype) {
    if (at == object) return false;
  }
  object->prototype = prototype;
  return true;
}

void csObjectLeaveShapeMode(ObjObject *object) {
  if (object->shape != NULL) convertToDictionary(object);
}

unsigned csObjectAttributes(ObjObject *object, ObjString *key) {
  if (object->attributes == NULL) return CS_PROP_DEFAULT;
  Value stored;
  if (!csTableGet(object->attributes, key, &stored)) return CS_PROP_DEFAULT;
  return (unsigned)AS_NUMBER(stored);
}

void csObjectSetAttributes(ObjObject *object, ObjString *key, unsigned attributes) {
  if (object->attributes == NULL) {
    if (attributes == CS_PROP_DEFAULT) return; /* nothing to record */
    csPushTempRoot((Obj *)object);
    csPushTempRoot((Obj *)key);
    Table *table = CS_ALLOCATE(Table, 1);
    csTableInit(table);
    object->attributes = table;
    csPopTempRoot();
    csPopTempRoot();
  }
  csTableSet(object->attributes, key, NUMBER_VAL((double)attributes));
}

bool csObjectIsEnumerable(ObjObject *object, ObjString *key) {
  /* The pointer test is the whole cost for an object nothing has defined a
   * property on, which is nearly all of them. */
  if (object->attributes == NULL) return true;
  return (csObjectAttributes(object, key) & CS_PROP_ENUMERABLE) != 0;
}

int csObjectCount(const ObjObject *object) {
  return object->shape != NULL ? object->shape->slotCount : object->as.dictionary.count;
}

ObjString *csObjectKeyAt(const ObjObject *object, int index) {
  return object->shape != NULL ? object->shape->keys[index] : object->as.dictionary.keys[index];
}

Value csObjectValueAt(ObjObject *object, int index) {
  if (object->shape != NULL) return object->as.slots.values[index];
  Value value;
  return csTableGet(&object->as.dictionary.table, object->as.dictionary.keys[index], &value) ? value : UNDEFINED_VAL;
}
