/* memory.h — the single allocation choke point and the mark-sweep collector.
 *
 * Every heap byte the interpreter owns passes through csReallocate, which is
 * what lets the collector track live bytes and decide when to run.
 */
#ifndef CSCRIPT_MEMORY_H
#define CSCRIPT_MEMORY_H

#include "cscript/common.h"
#include "cscript/value.h"

#define CS_GROW_CAPACITY(cap) ((cap) < 8 ? 8 : (cap) * 2)

#define CS_ALLOCATE(type, count) \
  ((type *)csReallocate(NULL, 0, sizeof(type) * (size_t)(count)))

#define CS_FREE(type, pointer) csReallocate(pointer, sizeof(type), 0)

#define CS_GROW_ARRAY(type, pointer, oldCount, newCount)     \
  ((type *)csReallocate(pointer, sizeof(type) * (size_t)(oldCount), \
                        sizeof(type) * (size_t)(newCount)))

#define CS_FREE_ARRAY(type, pointer, oldCount) \
  csReallocate(pointer, sizeof(type) * (size_t)(oldCount), 0)

/* Grow, shrink, allocate (oldSize 0) or free (newSize 0). Aborts on OOM. */
void *csReallocate(void *pointer, size_t oldSize, size_t newSize);

void csCollectGarbage(void);
void csFreeAllObjects(void);

/* Marking helpers, used by the root walkers. */
void csMarkObject(Obj *object);
void csMarkValue(Value value);

/* Protects a freshly allocated object that no root can reach yet — for the
 * window between allocating it and storing it somewhere the collector scans. */
void csPushTempRoot(Obj *object);
void csPopTempRoot(void);

#endif /* CSCRIPT_MEMORY_H */
