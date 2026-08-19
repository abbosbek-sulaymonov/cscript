/* compiler.h — lowers an AST into bytecode.
 *
 * The result is an ObjFunction wrapping the top level of the script, so the VM
 * runs a script and a call through exactly the same mechanism.
 */
#ifndef CSCRIPT_COMPILER_H
#define CSCRIPT_COMPILER_H

#include "cscript/ast.h"
#include "cscript/common.h"
#include "cscript/diagnostic.h"
#include "cscript/object.h"

/* Returns the compiled top-level function, or NULL if an error was reported. */
ObjFunction *csCompile(AstNode *program, Diagnostics *diag);

/* Marks the functions currently being compiled. Called by the collector. */
void csCompilerMarkRoots(void);

#endif /* CSCRIPT_COMPILER_H */
