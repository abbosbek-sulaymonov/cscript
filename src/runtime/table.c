#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/table.h"

/* Grow before the table gets dense: linear probing degrades badly past ~75%. */
#define TABLE_MAX_LOAD 0.75

void csTableInit(Table *table) {
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
  table->version = 0;
}

void csTableFree(Table *table) {
  CS_FREE_ARRAY(Entry, table->entries, table->capacity);
  csTableInit(table);
}

/* Returns the slot for `key`: either the entry holding it, or the slot it
 * should be inserted into. Reuses the first tombstone seen so deleted slots do
 * not leak, while still probing past it to find a real match. */
static Entry *findEntry(Entry *entries, int capacity, ObjString *key) {
  uint32_t index = key->hash & (uint32_t)(capacity - 1);
  Entry *tombstone = NULL;

  for (;;) {
    Entry *entry = &entries[index];
    if (entry->key == NULL) {
      if (IS_NULL(entry->value)) {
        /* Genuinely empty — the probe sequence ends here. */
        return tombstone != NULL ? tombstone : entry;
      }
      if (tombstone == NULL) tombstone = entry;
    } else if (entry->key == key) {
      /* Keys are interned, so pointer identity is string equality. */
      return entry;
    }
    index = (index + 1) & (uint32_t)(capacity - 1);
  }
}

static void adjustCapacity(Table *table, int capacity) {
  table->version++;
  Entry *entries = CS_ALLOCATE(Entry, capacity);
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NULL;
    entries[i].value = NULL_VAL;
  }

  /* Rehash live entries only; tombstones are dropped, so recount as we go. */
  table->count = 0;
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key == NULL) continue;

    Entry *dest = findEntry(entries, capacity, entry->key);
    dest->key = entry->key;
    dest->value = entry->value;
    table->count++;
  }

  CS_FREE_ARRAY(Entry, table->entries, table->capacity);
  table->entries = entries;
  table->capacity = capacity;
}

bool csTableGet(Table *table, ObjString *key, Value *out) {
  if (table->count == 0) return false;

  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) return false;

  if (out != NULL) *out = entry->value;
  return true;
}

Entry *csTableFindEntry(Table *table, ObjString *key) {
  if (table->count == 0) return NULL;
  Entry *entry = findEntry(table->entries, table->capacity, key);
  return entry->key == NULL ? NULL : entry;
}

bool csTableSet(Table *table, ObjString *key, Value value) {
  if ((double)(table->count + 1) > (double)table->capacity * TABLE_MAX_LOAD) {
    adjustCapacity(table, CS_GROW_CAPACITY(table->capacity));
  }

  Entry *entry = findEntry(table->entries, table->capacity, key);
  bool isNewKey = entry->key == NULL;
  /* Reusing a tombstone does not add to the count — it was already counted. */
  if (isNewKey && IS_NULL(entry->value)) table->count++;

  entry->key = key;
  entry->value = value;
  return isNewKey;
}

bool csTableDelete(Table *table, ObjString *key) {
  if (table->count == 0) return false;

  Entry *entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) return false;

  /* Tombstone: NULL key with a non-null value, so probes keep walking. */
  entry->key = NULL;
  entry->value = BOOL_VAL(true);
  table->version++;
  return true;
}

void csTableAddAll(Table *from, Table *to) {
  for (int i = 0; i < from->capacity; i++) {
    Entry *entry = &from->entries[i];
    if (entry->key != NULL) csTableSet(to, entry->key, entry->value);
  }
}

ObjString *csTableFindString(Table *table, const char *chars, int length,
                             uint32_t hash) {
  if (table->count == 0) return NULL;

  uint32_t index = hash & (uint32_t)(table->capacity - 1);
  for (;;) {
    Entry *entry = &table->entries[index];
    if (entry->key == NULL) {
      if (IS_NULL(entry->value)) return NULL; /* empty slot, not present */
    } else if (entry->key->length == length && entry->key->hash == hash &&
               memcmp(entry->key->chars, chars, (size_t)length) == 0) {
      return entry->key;
    }
    index = (index + 1) & (uint32_t)(table->capacity - 1);
  }
}

void csTableMark(Table *table) {
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    csMarkObject((Obj *)entry->key);
    csMarkValue(entry->value);
  }
}

/* Drops entries whose *value* did not survive. For the map from a symbol's
 * filing name back to the symbol: it must not be what keeps every symbol ever
 * made alive, and a symbol nothing holds has nothing left to be enumerated
 * by. */
void csTableRemoveWhiteValues(Table *table) {
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key == NULL) continue;
    if (!IS_OBJ(entry->value) || AS_OBJ(entry->value)->isMarked) continue;
    csTableDelete(table, entry->key);
  }
}

void csTableRemoveWhite(Table *table) {
  for (int i = 0; i < table->capacity; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key != NULL && !entry->key->obj.isMarked) {
      csTableDelete(table, entry->key);
    }
  }
}
