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

/* Returns false when a type error was reported.
 *
 * `sourcePath` is the file being checked, resolved and absolute, or NULL when
 * there is no file — `-e` and the REPL. It is what an `import` is resolved
 * against, so that the types of what a module exports reach the file that
 * imported it. */
bool csTypeCheck(AstNode *program, Diagnostics *diag, const char *sourcePath);

#endif /* CSCRIPT_TYPECHECK_H */
