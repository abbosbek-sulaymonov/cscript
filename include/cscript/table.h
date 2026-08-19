/* table.h — open-addressing hash table keyed by interned ObjString.
 *
 * Backs both the globals map and the string intern pool. Collisions probe
 * linearly and deletions leave a tombstone (a NULL key with a `true` value) so
 * probe sequences stay intact.
 */
#ifndef CSCRIPT_TABLE_H
#define CSCRIPT_TABLE_H

#include "cscript/common.h"
#include "cscript/value.h"

typedef struct {
  ObjString *key;
  Value value;
} Entry;

typedef struct {
  int count; /* live entries plus tombstones */
  int capacity;
  Entry *entries;
} Table;

void csTableInit(Table *table);
void csTableFree(Table *table);

bool csTableGet(Table *table, ObjString *key, Value *out);

/* Returns true when the key was newly inserted rather than overwritten. */
bool csTableSet(Table *table, ObjString *key, Value value);

bool csTableDelete(Table *table, ObjString *key);
void csTableAddAll(Table *from, Table *to);

/* Intern-pool lookup: finds by contents rather than by pointer. */
ObjString *csTableFindString(Table *table, const char *chars, int length,
                             uint32_t hash);

/* GC support. */
void csTableMark(Table *table);
void csTableRemoveWhite(Table *table);

#endif /* CSCRIPT_TABLE_H */
