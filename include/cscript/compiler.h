/* compiler.h — lowers an AST into a Chunk of bytecode. */
#ifndef CSCRIPT_COMPILER_H
#define CSCRIPT_COMPILER_H

#include "cscript/ast.h"
#include "cscript/chunk.h"
#include "cscript/common.h"
#include "cscript/diagnostic.h"

/* Writes bytecode for `program` into `chunk`. Returns false if it reported an
 * error. The chunk is registered as a GC root while it is being filled, because
 * interning string constants can allocate. */
bool csCompile(AstNode *program, Chunk *chunk, Diagnostics *diag);

/* Marks the chunk currently being compiled. Called by the collector. */
void csCompilerMarkRoots(void);

#endif /* CSCRIPT_COMPILER_H */
