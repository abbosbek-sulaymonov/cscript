/* module.h — loading, resolving and running source files.
 *
 * A module is one file and the scope its top level lives in. The loader sits
 * above the compiler rather than inside it, because the compiler is a single
 * global `current` and must not be re-entered: dependencies are read, parsed
 * and compiled *before* the file that imports them, so by the time the
 * compiler reaches an `import` the module it names is already there and a
 * missing export is a compile error rather than a surprise at run time.
 *
 * The traversal is post-order, so the execution list comes out in dependency
 * order and everything a module imports has already run when it starts.
 */
#ifndef CSCRIPT_MODULE_H
#define CSCRIPT_MODULE_H

#include "cscript/ast.h"
#include "cscript/common.h"
#include "cscript/diagnostic.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* Reads a whole file into a NUL-terminated buffer the caller owns, or returns
 * NULL after reporting why not. */
char *csReadFile(const char *path, bool quiet);

/* Turns `specifier` — always relative — into an absolute path, resolved
 * against the directory holding `fromPath`. Returns false when nothing is
 * there, which is the only way a specifier can fail to resolve. */
bool csModuleResolve(const char *fromPath, const char *specifier, char *out,
                     size_t outSize);

/* How a file is named in output: relative to where the program was started
 * when it sits underneath, absolute otherwise. The returned pointer points
 * into `path`, so it lives exactly as long as that does. */
const char *csModuleDisplayPath(const char *path);

/* The module already loaded for an absolute path, or NULL. */
ObjModule *csModuleFind(const char *resolvedPath);

/* Loads, parses and compiles a file and everything it imports, unless that has
 * already happened. Errors are reported against `from` when one is given — so
 * a missing file is reported at the import that named it — and printed plainly
 * when it is the entry file. */
ObjModule *csModuleLoadResolved(const char *resolvedPath, const char *shownAs,
                                Diagnostics *from, int line);

/* Loads every module the top level of `program` imports, resolved against
 * `fromPath`. The loader calls this for each file it reads; csInterpret calls
 * it so that `-e` and the REPL can import too, relative to the working
 * directory. */
bool csModuleLoadImports(const AstNode *program, const char *fromPath,
                         Diagnostics *diag);

/* Loads and runs a file as the entry point of a program. */
InterpretResult csRunFile(const char *path);

#endif /* CSCRIPT_MODULE_H */
