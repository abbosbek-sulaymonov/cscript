/* debug.h — disassembler and AST printer, used by the debug build flags. */
#ifndef CSCRIPT_DEBUG_H
#define CSCRIPT_DEBUG_H

#include "cscript/ast.h"
#include "cscript/chunk.h"

void csDisassembleChunk(const Chunk *chunk, const char *name);

/* Disassembles one instruction and returns the offset of the next. */
int csDisassembleInstruction(const Chunk *chunk, int offset);

/* The same walk with no output: where the next instruction starts. Shared so
 * that nothing else has to keep its own copy of the operand layout. */
int csInstructionLength(const Chunk *chunk, int offset);

void csAstPrint(const AstNode *node);

#endif /* CSCRIPT_DEBUG_H */
