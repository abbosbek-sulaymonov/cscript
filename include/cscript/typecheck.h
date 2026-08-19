/* typecheck.h — the static checking pass, between parsing and code generation.
 *
 * Walks the AST, resolves a type onto every expression node and reports
 * mismatches through the shared diagnostics object. Two things come out of it:
 * errors the programmer sees before the program runs, and type information the
 * compiler can specialise against.
 */
#ifndef CSCRIPT_TYPECHECK_H
#define CSCRIPT_TYPECHECK_H

#include "cscript/ast.h"
#include "cscript/common.h"
#include "cscript/diagnostic.h"

/* Returns false when a type error was reported. */
bool csTypeCheck(AstNode *program, Diagnostics *diag);

#endif /* CSCRIPT_TYPECHECK_H */
