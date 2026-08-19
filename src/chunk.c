#include "cscript/chunk.h"
#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/vm.h"

void csChunkInit(Chunk *chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  csValueArrayInit(&chunk->constants);
}

void csChunkFree(Chunk *chunk) {
  CS_FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  CS_FREE_ARRAY(int, chunk->lines, chunk->capacity);
  csValueArrayFree(&chunk->constants);
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
