/* debug.h — disassembler and AST printer, and the flags that ask for them.
 *
 * The printers themselves are in every build, because they are small and the
 * disassembler's instruction walk is also what the compiler's own passes use
 * to stride the bytecode. What used to be compile-time only was the *asking*:
 * `make trace` turned all of it on for the whole run and nothing else could
 * turn any of it on at all, so seeing one function's bytecode meant rebuilding
 * the interpreter. The stages are selectable at run time now — see the `-p`
 * options in `cscript --help` — and the trace build simply starts with them
 * all set.
 */
#ifndef CSCRIPT_DEBUG_H
#define CSCRIPT_DEBUG_H

#include "cscript/ast.h"
#include "cscript/chunk.h"
#include "cscript/common.h"

typedef enum {
  CS_DUMP_TOKENS = 1u << 0,
  CS_DUMP_AST = 1u << 1,
  CS_DUMP_BYTECODE = 1u << 2,
} CsDumpStage;

/* Which stages to print on the way past. Set from the command line before the
 * first source is read, and never afterwards. */
extern unsigned csDumpStages;

static inline bool csDumping(CsDumpStage stage) {
  return (csDumpStages & (unsigned)stage) != 0;
}

void csDisassembleChunk(const Chunk *chunk, const char *name);

/* Disassembles one instruction and returns the offset of the next. */
int csDisassembleInstruction(const Chunk *chunk, int offset);

/* The same walk with no output: where the next instruction starts. Shared so
 * that nothing else has to keep its own copy of the operand layout. */
int csInstructionLength(const Chunk *chunk, int offset);

void csAstPrint(const AstNode *node);

#endif /* CSCRIPT_DEBUG_H */
