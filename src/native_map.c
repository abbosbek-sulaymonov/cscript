/* native_map.c — Map and Set.
 *
 * A Set is a Map that only ever looks at its keys, so both are one structure
 * with a flag. Keeping two near-identical hash tables in step would cost more
 * than the branch does.
 *
 * Why not reuse Table. That one is keyed by interned ObjString, so a lookup is
 * a pointer compare and the hash is already computed. A Map takes *any* value
 * as a key, which means hashing numbers, booleans and object identity, and
 * comparing them by SameValueZero. Different enough to be its own thing.
 *
 * Why entries are a dense array with a separate index. JavaScript guarantees
 * that a Map iterates in insertion order, and that deleting an entry does not
 * disturb the order of the rest. A dense array gives both directly; the
 * open-addressing index beside it is what keeps lookup constant.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#define MAP_MAX_LOAD 0.75

bool csValuesSameValueZero(Value a, Value b) {
  /* NaN is the one value `===` says is not itself, and a Map that could not
   * find a NaN key it had just stored would be useless. +0 and -0 are the
   * mirror case: `===` says they match, and so does this. */
  if (IS_NUMBER(a) && IS_NUMBER(b)) {
    double x = AS_NUMBER(a);
    double y = AS_NUMBER(b);
    if (x != x && y != y) return true;
    return x == y;
  }
  return csValuesStrictEqual(a, b);
}

static uint32_t hashValue(Value value) {
  if (IS_STRING(value)) return AS_STRING(value)->hash;
  if (IS_NUMBER(value)) {
    double number = AS_NUMBER(value);
    /* Every NaN hashes alike, and -0 hashes as +0, so the two special cases
     * SameValueZero cares about land in the same bucket as their partner. */
    if (number != number) return 0x7ff80000u;
    if (number == 0) number = 0;
    uint64_t bits;
    memcpy(&bits, &number, sizeof bits);
    return (uint32_t)(bits ^ (bits >> 32));
  }
  if (IS_BOOL(value)) return AS_BOOL(value) ? 1u : 2u;
  if (IS_NULL(value)) return 3u;
  if (IS_UNDEFINED(value)) return 4u;

  /* Everything else is identity, which is what JavaScript uses too: two
   * objects with the same contents are different keys. */
  uintptr_t address = (uintptr_t)AS_OBJ(value);
  return (uint32_t)(address ^ (address >> 32));
}

/* The index slot for `key`: either where it lives, or where it would go. */
static int findSlot(const ObjMap *map, Value key) {
  uint32_t mask = (uint32_t)map->indexCapacity - 1;
  uint32_t slot = hashValue(key) & mask;
  for (;;) {
    int entry = map->index[slot];
    if (entry == -1) return (int)slot;
    if (map->entries[entry].present &&
        csValuesSameValueZero(map->entries[entry].key, key)) {
      return (int)slot;
    }
    slot = (slot + 1) & mask;
  }
}

/* Rebuilds the index, and compacts the entries so tombstones do not accumulate
 * in a map that is repeatedly added to and deleted from. */
static void rehash(ObjMap *map, int indexCapacity) {
  int live = 0;
  for (int i = 0; i < map->count; i++) {
    if (map->entries[i].present) map->entries[live++] = map->entries[i];
  }
  map->count = live;

  int *index = CS_ALLOCATE(int, indexCapacity);
  for (int i = 0; i < indexCapacity; i++) index[i] = -1;

  int *old = map->index;
  int oldCapacity = map->indexCapacity;
  map->index = index;
  map->indexCapacity = indexCapacity;

  for (int i = 0; i < map->count; i++) {
    map->index[findSlot(map, map->entries[i].key)] = i;
  }
  CS_FREE_ARRAY(int, old, oldCapacity);
}

ObjMap *csMapNew(bool isSet) {
  ObjMap *map = CS_ALLOCATE(ObjMap, 1);
  map->obj.type = OBJ_MAP;
  map->obj.isMarked = false;
  map->obj.next = vm.objects;
  vm.objects = (Obj *)map;

  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
  map->liveCount = 0;
  map->index = NULL;
  map->indexCapacity = 0;
  map->isSet = isSet;
  return map;
}

bool csMapGet(ObjMap *map, Value key, Value *out) {
  if (map->indexCapacity == 0) return false;
  int entry = map->index[findSlot(map, key)];
  if (entry == -1) return false;
  if (out != NULL) *out = map->entries[entry].value;
  return true;
}

bool csMapHas(ObjMap *map, Value key) { return csMapGet(map, key, NULL); }

void csMapSet(ObjMap *map, Value key, Value value) {
  if (map->indexCapacity == 0) rehash(map, 8);

  int slot = findSlot(map, key);
  int entry = map->index[slot];
  if (entry != -1) {
    map->entries[entry].value = value;
    return;
  }

  /* Growing keys off the *entry* count rather than the live count, because a
   * map full of tombstones needs the compaction as much as a full one does. */
  if ((double)(map->count + 1) > (double)map->indexCapacity * MAP_MAX_LOAD) {
    rehash(map, map->indexCapacity * 2);
    slot = findSlot(map, key);
  }

  if (map->capacity < map->count + 1) {
    int oldCapacity = map->capacity;
    map->capacity = CS_GROW_CAPACITY(oldCapacity);
    map->entries = CS_GROW_ARRAY(MapEntry, map->entries, oldCapacity, map->capacity);
  }

  map->entries[map->count].key = key;
  map->entries[map->count].value = value;
  map->entries[map->count].present = true;
  map->index[slot] = map->count;
  map->count++;
  map->liveCount++;
}

bool csMapDelete(ObjMap *map, Value key) {
  if (map->indexCapacity == 0) return false;
  int slot = findSlot(map, key);
  int entry = map->index[slot];
  if (entry == -1) return false;

  /* The entry keeps its place so the order of everything after it is
   * undisturbed; the index forgets it so a later insert can reuse the key. */
  map->entries[entry].present = false;
  map->entries[entry].key = UNDEFINED_VAL;
  map->entries[entry].value = UNDEFINED_VAL;
  map->liveCount--;

  /* Re-index rather than tombstone the slot: the probe sequences after it have
   * to stay walkable, and rebuilding is simpler to get right than a second
   * kind of empty. */
  rehash(map, map->indexCapacity);
  return true;
}

void csMapClear(ObjMap *map) {
  map->count = 0;
  map->liveCount = 0;
  for (int i = 0; i < map->indexCapacity; i++) map->index[i] = -1;
}

ObjArray *csMapToArray(ObjMap *map) {
  ObjArray *items = csArrayNew();
  csPushTempRoot((Obj *)items);

  for (int i = 0; i < map->count; i++) {
    if (!map->entries[i].present) continue;

    if (map->isSet) {
      Value element = map->entries[i].key;
      if (IS_OBJ(element)) csPushTempRoot(AS_OBJ(element));
      csValueArrayWrite(&items->elements, element);
      if (IS_OBJ(element)) csPopTempRoot();
      continue;
    }

    ObjArray *pair = csArrayNew();
    csPushTempRoot((Obj *)pair);
    csValueArrayWrite(&pair->elements, map->entries[i].key);
    csValueArrayWrite(&pair->elements, map->entries[i].value);
    csValueArrayWrite(&items->elements, OBJ_VAL(pair));
    csPopTempRoot();
  }

  csPopTempRoot();
  return items;
}

/* ---- the methods ------------------------------------------------------- */

static bool requireMap(Value receiver, const char *method, bool wantSet) {
  if (IS_MAP(receiver) && AS_MAP(receiver)->isSet == wantSet) return true;
  csVMRuntimeError("'%s' can only be called on a %s", method, wantSet ? "Set" : "Map");
  return false;
}

static bool mapGet(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireMap(receiver, "get", false)) return false;
  Value key = argCount > 0 ? args[0] : UNDEFINED_VAL;
  /* A missing key and a key stored as undefined both read as undefined, which
   * is why `has` exists separately. */
  if (!csMapGet(AS_MAP(receiver), key, result)) *result = UNDEFINED_VAL;
  return true;
}

static bool mapSet(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireMap(receiver, "set", false)) return false;
  csMapSet(AS_MAP(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL,
           argCount > 1 ? args[1] : UNDEFINED_VAL);
  *result = receiver; /* chainable, as in JavaScript */
  return true;
}

static bool setAdd(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireMap(receiver, "add", true)) return false;
  Value value = argCount > 0 ? args[0] : UNDEFINED_VAL;
  csMapSet(AS_MAP(receiver), value, value);
  *result = receiver;
  return true;
}

static bool mapHas(Value receiver, int argCount, Value *args, Value *result) {
  if (!IS_MAP(receiver)) {
    csVMRuntimeError("'has' can only be called on a Map or a Set");
    return false;
  }
  *result = BOOL_VAL(csMapHas(AS_MAP(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL));
  return true;
}

static bool mapDelete(Value receiver, int argCount, Value *args, Value *result) {
  if (!IS_MAP(receiver)) {
    csVMRuntimeError("'delete' can only be called on a Map or a Set");
    return false;
  }
  *result =
      BOOL_VAL(csMapDelete(AS_MAP(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL));
  return true;
}

static bool mapClear(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  if (!IS_MAP(receiver)) {
    csVMRuntimeError("'clear' can only be called on a Map or a Set");
    return false;
  }
  csMapClear(AS_MAP(receiver));
  *result = UNDEFINED_VAL;
  return true;
}

/* keys, values and entries all walk the same array; what they build differs. */
typedef enum { WANT_KEYS, WANT_VALUES, WANT_ENTRIES } Want;

static bool collect(Value receiver, Value *result, Want want, const char *method) {
  if (!IS_MAP(receiver)) {
    csVMRuntimeError("'%s' can only be called on a Map or a Set", method);
    return false;
  }
  ObjMap *map = AS_MAP(receiver);

  ObjArray *out = csArrayNew();
  csPushTempRoot((Obj *)out);

  for (int i = 0; i < map->count; i++) {
    if (!map->entries[i].present) continue;

    if (want == WANT_ENTRIES) {
      ObjArray *pair = csArrayNew();
      csPushTempRoot((Obj *)pair);
      csValueArrayWrite(&pair->elements, map->entries[i].key);
      csValueArrayWrite(&pair->elements, map->entries[i].value);
      csValueArrayWrite(&out->elements, OBJ_VAL(pair));
      csPopTempRoot();
      continue;
    }

    /* The element is a bare Value at this point, so it has to be rooted across
     * the append — which allocates. */
    Value element = want == WANT_KEYS ? map->entries[i].key : map->entries[i].value;
    if (IS_OBJ(element)) csPushTempRoot(AS_OBJ(element));
    csValueArrayWrite(&out->elements, element);
    if (IS_OBJ(element)) csPopTempRoot();
  }

  csPopTempRoot();
  *result = OBJ_VAL(out);
  return true;
}

static bool mapKeys(Value r, int c, Value *a, Value *out) {
  (void)c; (void)a;
  return collect(r, out, WANT_KEYS, "keys");
}
static bool mapValues(Value r, int c, Value *a, Value *out) {
  (void)c; (void)a;
  return collect(r, out, WANT_VALUES, "values");
}
static bool mapEntries(Value r, int c, Value *a, Value *out) {
  (void)c; (void)a;
  /* A Set's entries are [v, v] in JavaScript, and storing the value as its own
   * key is what makes that fall out rather than needing a case. */
  return collect(r, out, WANT_ENTRIES, "entries");
}

static bool mapForEach(Value receiver, int argCount, Value *args, Value *result) {
  if (!IS_MAP(receiver) || argCount < 1) {
    csVMRuntimeError("forEach expects a function and a Map or Set receiver");
    return false;
  }
  ObjMap *map = AS_MAP(receiver);

  for (int i = 0; i < map->count; i++) {
    if (!map->entries[i].present) continue;
    /* (value, key, collection), as JavaScript passes them. */
    Value callArgs[3] = {map->entries[i].value, map->entries[i].key, receiver};
    Value ignored;
    if (!csVMCallAdapted(args[0], callArgs, 3, &ignored)) return false;
  }

  *result = UNDEFINED_VAL;
  return true;
}

/* ---- construction ------------------------------------------------------ */

/* `new Map([[k, v], ...])` and `new Set([...])`. Both accept nothing, and both
 * accept an array — which is how one is copied. */
static bool construct(int argCount, Value *args, Value *result, bool isSet) {
  ObjMap *map = csMapNew(isSet);
  csPushTempRoot((Obj *)map);

  if (argCount > 0 && !IS_NULL(args[0]) && !IS_UNDEFINED(args[0])) {
    if (!IS_ARRAY(args[0])) {
      csPopTempRoot();
      csVMRuntimeError("%s expects an array", isSet ? "Set" : "Map");
      return false;
    }
    ObjArray *source = AS_ARRAY(args[0]);
    for (int i = 0; i < source->elements.count; i++) {
      Value element = source->elements.values[i];
      if (isSet) {
        csMapSet(map, element, element);
        continue;
      }
      if (!IS_ARRAY(element) || AS_ARRAY(element)->elements.count < 2) {
        csPopTempRoot();
        csVMRuntimeError("Map expects an array of [key, value] pairs");
        return false;
      }
      csMapSet(map, AS_ARRAY(element)->elements.values[0],
               AS_ARRAY(element)->elements.values[1]);
    }
  }

  csPopTempRoot();
  *result = OBJ_VAL(map);
  return true;
}

static bool mapConstruct(Value r, int c, Value *a, Value *out) {
  (void)r;
  return construct(c, a, out, false);
}
static bool setConstruct(Value r, int c, Value *a, Value *out) {
  (void)r;
  return construct(c, a, out, true);
}

static void defineMapMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.mapMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csMapMethodsInstall(void) {
  defineMapMethod("get", mapGet, -1);
  defineMapMethod("set", mapSet, -1);
  defineMapMethod("add", setAdd, -1);
  defineMapMethod("has", mapHas, -1);
  defineMapMethod("delete", mapDelete, -1);
  defineMapMethod("clear", mapClear, 0);
  defineMapMethod("keys", mapKeys, 0);
  defineMapMethod("values", mapValues, 0);
  defineMapMethod("entries", mapEntries, 0);
  defineMapMethod("forEach", mapForEach, -1);
}

NativeFn csMapConstructorFn(void) { return mapConstruct; }
NativeFn csSetConstructorFn(void) { return setConstruct; }
