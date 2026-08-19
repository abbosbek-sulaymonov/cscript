#include "cscript/chunk.h"
#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/vm.h"

void csChunkInit(Chunk *chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  csValueArrayInit(&chunk->constants);
  chunk->propertyCaches = NULL;
  chunk->propertyCacheCount = 0;
  chunk->globalCaches = NULL;
  chunk->globalCacheCount = 0;
}

void csChunkFree(Chunk *chunk) {
  CS_FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  CS_FREE_ARRAY(int, chunk->lines, chunk->capacity);
  csValueArrayFree(&chunk->constants);
  CS_FREE_ARRAY(PropertyCache, chunk->propertyCaches, chunk->propertyCacheCount);
  CS_FREE_ARRAY(GlobalCache, chunk->globalCaches, chunk->globalCacheCount);
  csChunkInit(chunk);
}

void csChunkWrite(Chunk *chunk, uint8_t byte, int line) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = CS_GROW_CAPACITY(oldCapacity);
    chunk->code = CS_GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
    chunk->lines = CS_GROW_ARRAY(int, chunk->lines, oldCapacity, chunk->capacity);
  }
  chunk->code[chunk->count] = byte;
  chunk->lines[chunk->count] = line;
  chunk->count++;
}

int csChunkAddConstant(Chunk *chunk, Value value) {
  /* Growing the constant array allocates, which can trigger a collection while
   * `value` is not yet reachable from any root. */
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  csValueArrayWrite(&chunk->constants, value);
  if (IS_OBJ(value)) csPopTempRoot();

  return chunk->constants.count - 1;
}

/* Sized exactly, one slot at a time, because the compiler asks for one per
 * site and never for more. The reallocation cost is paid at compile time and
 * the array is then read once per execution of the site. */
/* The count is published last. Growing allocates, and a collection triggered
 * in the middle walks this array to prune dead shapes — so the count must
 * never describe more entries than the array actually holds. */
int csChunkAddPropertyCache(Chunk *chunk) {
  int index = chunk->propertyCacheCount;
  chunk->propertyCaches =
      CS_GROW_ARRAY(PropertyCache, chunk->propertyCaches, index, index + 1);
  chunk->propertyCaches[index].shape = vm.absentShape;
  chunk->propertyCaches[index].slot = 0;
  chunk->propertyCacheCount = index + 1;
  return index;
}

int csChunkAddGlobalCache(Chunk *chunk) {
  int index = chunk->globalCacheCount;
  chunk->globalCaches =
      CS_GROW_ARRAY(GlobalCache, chunk->globalCaches, index, index + 1);
  chunk->globalCaches[index].entry = NULL;
  chunk->globalCaches[index].version = 0;
  chunk->globalCaches[index].filled = false;
  chunk->globalCacheCount = index + 1;
  return index;
}

void csChunkPruneCaches(Chunk *chunk) {
  /* A cached shape is a weak reference: caching a layout must not be what
   * keeps it alive, or a program that builds many short-lived layouts would
   * retain every one of them for as long as the code that touched them. */
  for (int i = 0; i < chunk->propertyCacheCount; i++) {
    Shape *shape = chunk->propertyCaches[i].shape;
    if (!shape->obj.isMarked) chunk->propertyCaches[i].shape = vm.absentShape;
  }
}
