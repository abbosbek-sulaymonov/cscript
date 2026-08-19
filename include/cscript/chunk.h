/* chunk.h — a compiled unit: bytecode, its constant pool and line mapping. */
#ifndef CSCRIPT_CHUNK_H
#define CSCRIPT_CHUNK_H

#include "cscript/common.h"
#include "cscript/opcode.h"
#include "cscript/value.h"

typedef struct {
  int count;
  int capacity;
  uint8_t *code;
  int *lines; /* parallel to `code`: source line per byte, for stack traces */
  ValueArray constants;
} Chunk;

void csChunkInit(Chunk *chunk);
void csChunkFree(Chunk *chunk);
void csChunkWrite(Chunk *chunk, uint8_t byte, int line);

/* Appends to the constant pool and returns its index. The value is protected
 * from collection for the duration of the call. */
int csChunkAddConstant(Chunk *chunk, Value value);

#endif /* CSCRIPT_CHUNK_H */
