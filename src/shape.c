#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

/* Shapes are heap objects so the collector reclaims layouts a program stopped
 * using, but they are never visible from CScript. */
static Shape *allocateShape(void) {
  Shape *shape = CS_ALLOCATE(Shape, 1);
  shape->obj.type = OBJ_SHAPE;
  shape->obj.isMarked = false;
  shape->obj.next = vm.objects;
  vm.objects = (Obj *)shape;

  shape->parent = NULL;
  shape->key = NULL;
  shape->slotCount = 0;
  shape->keys = NULL;
  csTableInit(&shape->indices);
  csTableInit(&shape->transitions);
  return shape;
}

Shape *csShapeNewRoot(void) { return allocateShape(); }

bool csShapeLookup(Shape *shape, ObjString *key, int *slot) {
  Value index;
  if (!csTableGet(&shape->indices, key, &index)) return false;
  *slot = (int)AS_NUMBER(index);
  return true;
}

Shape *csShapeTransition(Shape *parent, ObjString *key) {
  Value existing;
  if (csTableGet(&parent->transitions, key, &existing)) {
    return (Shape *)AS_OBJ(existing);
  }

  if (parent->slotCount >= CS_SHAPE_MAX_SLOTS) return NULL;

  /* Everything below allocates, and the only reference to the new shape is a C
   * local until the transition is recorded. */
  csPushTempRoot((Obj *)parent);
  csPushTempRoot((Obj *)key);

  Shape *child = allocateShape();
  csPushTempRoot((Obj *)child);

  child->parent = parent;
  child->key = key;

  /* slotCount is what the collector uses to size its walk of `keys`, so it
   * stays at zero until that array exists and is filled. Every line below can
   * collect. */
  int slotCount = parent->slotCount + 1;
  child->keys = CS_ALLOCATE(ObjString *, slotCount);
  if (parent->slotCount > 0) {
    memcpy(child->keys, parent->keys, sizeof(ObjString *) * (size_t)parent->slotCount);
  }
  child->keys[parent->slotCount] = key;
  child->slotCount = slotCount;

  /* Copying the parent's map costs O(n) once per distinct layout, and buys an
   * O(1) miss path for every object that ever has this shape. */
  csTableAddAll(&parent->indices, &child->indices);
  csTableSet(&child->indices, key, NUMBER_VAL(parent->slotCount));

  csTableSet(&parent->transitions, key, OBJ_VAL(child));

  csPopTempRoot();
  csPopTempRoot();
  csPopTempRoot();
  return child;
}

void csShapePruneTransitions(Shape *shape) {
  for (int i = 0; i < shape->transitions.capacity; i++) {
    Entry *entry = &shape->transitions.entries[i];
    if (entry->key == NULL) continue;
    if (!AS_OBJ(entry->value)->isMarked) {
      csTableDelete(&shape->transitions, entry->key);
    }
  }
}

void csShapeBlacken(Shape *shape) {
  csMarkObject((Obj *)shape->parent);
  csMarkObject((Obj *)shape->key);
  for (int i = 0; i < shape->slotCount; i++) {
    csMarkObject((Obj *)shape->keys[i]);
  }
  /* `indices` maps names to numbers, so only its keys are references — and
   * `keys` above already covers exactly those strings. `transitions` is weak
   * and is pruned rather than marked. */
}

void csShapeFree(Shape *shape) {
  csTableFree(&shape->indices);
  csTableFree(&shape->transitions);
  CS_FREE_ARRAY(ObjString *, shape->keys, shape->slotCount);
  CS_FREE(Shape, shape);
}
