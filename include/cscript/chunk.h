/* chunk.h — a compiled unit: bytecode, its constant pool and line mapping. */
#ifndef CSCRIPT_CHUNK_H
#define CSCRIPT_CHUNK_H

#include "cscript/common.h"
#include "cscript/opcode.h"
#include "cscript/table.h"
#include "cscript/value.h"

/* What one property site learned last time it ran.
 *
 * Monomorphic on purpose: one entry, filled on the first execution and
 * replaced whenever a different shape arrives. A site that genuinely sees many
 * shapes falls back to the hash lookup, which is exactly what it cost before
 * caches existed — so the worst case is no worse, and the common case, where a
 * site only ever sees objects built by one literal, is a compare and a load. */
typedef struct {
  Shape *shape; /* vm.absentShape until filled; never NULL */
  int slot;
} PropertyCache;

/* What one global site learned. A globals table never moves an Entry except on
 * a rehash or a delete, both of which bump the table's version — so a matching
 * version means the pointer is still the right one.
 *
 * The table is part of the cache rather than looked up from the running frame:
 * a site lives in exactly one module's code, so the table it resolves against
 * is fixed for the life of the program. That is what keeps per-module scope
 * from costing anything on the hot path. */
typedef struct {
  Table *table;
  Entry *entry;
  uint32_t version;
  bool filled;
} GlobalCache;

typedef struct {
  int count;
  int capacity;
  uint8_t *code;
  int *lines; /* parallel to `code`: source line per byte, for stack traces */
  ValueArray constants;

  /* Caches live beside the code rather than inside it. The collector has to
   * find every cached shape, and walking a flat array is a great deal safer
   * than decoding instructions to locate operands. */
  PropertyCache *propertyCaches;
  int propertyCacheCount;
  GlobalCache *globalCaches;
  int globalCacheCount;
} Chunk;

void csChunkInit(Chunk *chunk);
void csChunkFree(Chunk *chunk);
void csChunkWrite(Chunk *chunk, uint8_t byte, int line);

/* Appends to the constant pool and returns its index. The value is protected
 * from collection for the duration of the call. */
int csChunkAddConstant(Chunk *chunk, Value value);

/* Reserves a cache slot for one site and returns its index, which the compiler
 * emits as the instruction's operand. */
int csChunkAddPropertyCache(Chunk *chunk);
int csChunkAddGlobalCache(Chunk *chunk);

/* Drops cached shapes that did not survive the last collection. Called during
 * GC, before sweeping frees them. */
void csChunkPruneCaches(Chunk *chunk);

#endif /* CSCRIPT_CHUNK_H */
