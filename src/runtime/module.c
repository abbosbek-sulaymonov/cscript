/* module.c — finding a file, compiling it, and the entry point.
 *
 * A specifier is resolved against the importing file, and imports are loaded
 * depth-first: by the time a module is compiled, every module it imports
 * already exists, which is what makes a missing export a compile error rather
 * than a runtime one. A module is cached under its resolved path, so a diamond
 * of imports runs the shared file once.
 *
 * One kind of specifier is not a path at all. `std:iter` names a module in the
 * library that ships with the language, and has to mean the same file whatever
 * directory the program was run from — so it is resolved against the
 * executable's own location rather than against the importer.
 */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "cscript/ast.h"
#include "cscript/compiler.h"
#include "cscript/debug.h"
#include "cscript/lexer.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/parser.h"
#include "cscript/typecheck.h"
#include "cscript/vm.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *csReadFile(const char *path, bool quiet) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    if (!quiet) fprintf(stderr, "cscript: could not open '%s'\n", path);
    return NULL;
  }

  fseek(file, 0L, SEEK_END);
  long size = ftell(file);
  rewind(file);
  if (size < 0) {
    if (!quiet) fprintf(stderr, "cscript: could not measure '%s'\n", path);
    fclose(file);
    return NULL;
  }

  char *buffer = (char *)malloc((size_t)size + 1);
  if (buffer == NULL) {
    if (!quiet) fprintf(stderr, "cscript: not enough memory to read '%s'\n", path);
    fclose(file);
    return NULL;
  }

  size_t read = fread(buffer, sizeof(char), (size_t)size, file);
  if (read < (size_t)size) {
    if (!quiet) fprintf(stderr, "cscript: could not read '%s'\n", path);
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[read] = '\0';

  fclose(file);
  return buffer;
}

/* The directory part of a path, or "." when there is none. */
static void directoryOf(const char *path, char *out, size_t size) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    snprintf(out, size, ".");
    return;
  }
  size_t length = (size_t)(slash - path);
  if (length == 0) {
    snprintf(out, size, "/");
    return;
  }
  if (length >= size) length = size - 1;
  memcpy(out, path, length);
  out[length] = '\0';
}

/* ---- the standard library ----------------------------------------------- */

#define CS_STD_PREFIX "std:"
#define CS_STD_PREFIX_LENGTH 4

/* Where this binary is, which is the one thing an installed copy and a source
 * checkout both know about themselves. False where the platform has no way to
 * ask, in which case only CSCRIPT_STD_PATH can find the library. */
static bool executableDirectory(char *out, size_t size) {
  char path[PATH_MAX];
#if defined(__APPLE__)
  uint32_t length = (uint32_t)sizeof path;
  if (_NSGetExecutablePath(path, &length) != 0) return false;
#elif defined(__linux__)
  ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
  if (length <= 0) return false;
  path[length] = '\0';
#else
  (void)path;
  return false;
#endif

  char resolved[PATH_MAX];
  if (realpath(path, resolved) == NULL) return false;
  directoryOf(resolved, out, size);
  return true;
}

/* `<directory>/<name>/<name>.cx`, if there is such a file.
 *
 * A module is a directory rather than a file, so that it can carry its own
 * README beside its source — the specification of what it offers, where the
 * source is what it does. The name is repeated rather than the file being
 * called `index` or `mod`, so that a stack frame or a grep names the module
 * and not the twenty files that would otherwise share a name. */
static bool stdModuleIn(const char *directory, const char *name, char *out,
                        size_t outSize) {
  char candidate[PATH_MAX];
  if (snprintf(candidate, sizeof candidate, "%s/%s/%s.cx", directory, name, name) >=
      (int)sizeof candidate) {
    return false;
  }

  char resolved[PATH_MAX];
  if (realpath(candidate, resolved) == NULL) return false;

  size_t length = strlen(resolved);
  if (length >= outSize) return false;
  memcpy(out, resolved, length + 1);
  return true;
}

/* Where the library is.
 *
 * CSCRIPT_STD_PATH first, so a copy under test can be pointed at the library
 * it is testing rather than at whichever one is installed. Then three places
 * relative to the binary: an installed tree, a source checkout — where the
 * binary is build/<configuration>/cscript — and a binary sitting beside the
 * library directory. The first that exists wins, whatever is in it. */
static bool stdLibraryDirectory(char *out, size_t outSize) {
  /* Set and wrong is not the same as unset: falling back to another library
   * would answer with a file the environment explicitly said not to use. */
  const char *override = getenv("CSCRIPT_STD_PATH");
  if (override != NULL) {
    char resolved[PATH_MAX];
    if (realpath(override, resolved) == NULL) return false;
    size_t length = strlen(resolved);
    if (length >= outSize) return false;
    memcpy(out, resolved, length + 1);
    return true;
  }

  char executable[PATH_MAX];
  if (!executableDirectory(executable, sizeof executable)) return false;

  static const char *const relative[] = {"../lib/cscript", "../../library", "library"};
  for (size_t i = 0; i < sizeof relative / sizeof relative[0]; i++) {
    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof candidate, "%s/%s", executable, relative[i]) >=
        (int)sizeof candidate) {
      continue;
    }
    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) continue;
    size_t length = strlen(resolved);
    if (length >= outSize) return false;
    memcpy(out, resolved, length + 1);
    return true;
  }
  return false;
}

/* The file behind `std:<name>`. */
static bool stdModulePath(const char *name, char *out, size_t outSize) {
  /* A module name, not a path. Refusing the separators and the dot is what
   * stops `std:../../etc/passwd` from being a way out of the library. */
  if (*name == '\0') return false;
  for (const char *at = name; *at != '\0'; at++) {
    if (*at == '/' || *at == '\\' || *at == '.') return false;
  }

  char directory[PATH_MAX];
  if (!stdLibraryDirectory(directory, sizeof directory)) return false;
  return stdModuleIn(directory, name, out, outSize);
}

const char *csModuleResolutionHint(const char *specifier) {
  if (strncmp(specifier, CS_STD_PREFIX, CS_STD_PREFIX_LENGTH) != 0) return "";

  /* Two different failures wear the same message otherwise: a library that is
   * not there at all, and a library that is there without this module in it. */
  static char hint[PATH_MAX + 96];
  char directory[PATH_MAX];
  if (!stdLibraryDirectory(directory, sizeof directory)) {
    const char *override = getenv("CSCRIPT_STD_PATH");
    if (override != NULL) {
      snprintf(hint, sizeof hint,
               " — CSCRIPT_STD_PATH is set to '%s', which is not a directory",
               override);
      return hint;
    }
    return " — the standard library was not found; set CSCRIPT_STD_PATH to the"
           " directory holding it";
  }
  snprintf(hint, sizeof hint, " — no module of that name in the standard library at %s",
           directory);
  return hint;
}

bool csModuleResolve(const char *fromPath, const char *specifier, char *out,
                     size_t outSize) {
  if (strncmp(specifier, CS_STD_PREFIX, CS_STD_PREFIX_LENGTH) == 0) {
    return stdModulePath(specifier + CS_STD_PREFIX_LENGTH, out, outSize);
  }

  char directory[PATH_MAX];
  directoryOf(fromPath, directory, sizeof directory);

  char joined[PATH_MAX];
  if (snprintf(joined, sizeof joined, "%s/%s", directory, specifier) >=
      (int)sizeof joined) {
    return false;
  }

  /* realpath both normalises `..` and confirms the file exists, so a specifier
   * that resolves is one that can be read. */
  char resolved[PATH_MAX];
  if (realpath(joined, resolved) == NULL) return false;

  size_t length = strlen(resolved);
  if (length >= outSize) return false;
  memcpy(out, resolved, length + 1);
  return true;
}

const char *csModuleDisplayPath(const char *path) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof cwd) == NULL) return path;

  size_t length = strlen(cwd);
  if (strncmp(path, cwd, length) == 0 && path[length] == '/') return path + length + 1;
  return path;
}

ObjModule *csModuleFind(const char *resolvedPath) {
  ObjString *key = csStringCopy(resolvedPath, (int)strlen(resolvedPath));
  Value existing;
  if (!csTableGet(&vm.modules, key, &existing)) return NULL;
  return AS_MODULE(existing);
}

/* Reports against the importing file when there is one, so a bad import points
 * at the line that wrote it rather than at the file that could not be found. */
static void moduleError(Diagnostics *from, int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  if (from != NULL) {
    char message[512];
    vsnprintf(message, sizeof message, format, args);
    csDiagnosticError(from, line, NULL, 0, "%s", message);
  } else {
    fprintf(stderr, "cscript: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
  }
  va_end(args);
}

/* Loads every module the top level of `program` imports. Depth-first, so by
 * the time this returns every dependency is compiled and registered. */
bool csModuleLoadImports(const AstNode *program, const char *fromPath,
                         Diagnostics *diag) {
  if (program == NULL || program->type != AST_PROGRAM) return true;

  bool ok = true;
  for (int i = 0; i < program->as.program.count; i++) {
    const AstNode *statement = program->as.program.statements[i];
    if (statement == NULL || statement->type != AST_IMPORT) continue;

    char resolved[PATH_MAX];
    if (!csModuleResolve(fromPath, statement->as.import.specifier, resolved,
                         sizeof resolved)) {
      csDiagnosticError(diag, statement->line, NULL, 0, "cannot find module '%s'%s",
                        statement->as.import.specifier,
                        csModuleResolutionHint(statement->as.import.specifier));
      ok = false;
      continue;
    }

    if (csModuleLoadResolved(resolved, statement->as.import.specifier, diag,
                             statement->line) == NULL) {
      ok = false;
    }
  }
  return ok;
}

ObjModule *csModuleLoadResolved(const char *resolvedPath, const char *shownAs,
                                Diagnostics *from, int line) {
  ObjString *key = csStringCopy(resolvedPath, (int)strlen(resolvedPath));
  csPushTempRoot((Obj *)key);

  Value existing;
  if (csTableGet(&vm.modules, key, &existing)) {
    ObjModule *found = AS_MODULE(existing);
    csPopTempRoot();
    if (found->loading) {
      /* Named as it was written rather than as it resolved: the absolute path
       * is longer and says less about which line to go and look at. */
      moduleError(from, line, "import cycle: '%s' imports something that is "
                              "already being loaded", shownAs);
      return NULL;
    }
    return found;
  }

  char *source = csReadFile(resolvedPath, from != NULL);
  if (source == NULL) {
    csPopTempRoot();
    if (from != NULL) moduleError(from, line, "cannot read module '%s'", shownAs);
    return NULL;
  }

  if (vm.pendingCount >= CS_MODULES_MAX) {
    csPopTempRoot();
    free(source);
    moduleError(from, line, "too many modules in one program (limit %d)",
                CS_MODULES_MAX);
    return NULL;
  }

  ObjModule *module = csModuleNew(key);
  module->loading = true;
  csPushTempRoot((Obj *)module);
  /* Registered before its dependencies are read, which is what lets a cycle be
   * recognised rather than followed. */
  csTableSet(&vm.modules, key, OBJ_VAL(module));

  Diagnostics diag;
  csDiagnosticsInit(&diag, source, csModuleDisplayPath(module->path->chars));

  AstArena arena;
  csAstArenaInit(&arena);

  /* The stages the command line asked to see, for every file the program is
   * made of rather than only for the one `-e` was given. */
  if (csDumping(CS_DUMP_TOKENS)) {
    csLexerDumpTokens(source, &diag);
    csDiagnosticsInit(&diag, source, csModuleDisplayPath(module->path->chars));
  }

  AstNode *program = csParse(source, &arena, &diag);
  bool ok = program != NULL;
  if (ok) ok = csModuleLoadImports(program, resolvedPath, &diag);
  if (ok) ok = csTypeCheck(program, &diag);
  if (ok && csDumping(CS_DUMP_AST)) csAstPrint(program);

  ObjFunction *body = ok ? csCompile(program, module, &diag) : NULL;
  if (body != NULL && csDumping(CS_DUMP_BYTECODE)) {
    csDisassembleChunk(&body->chunk, csModuleDisplayPath(module->path->chars));
  }

  csAstArenaFree(&arena);
  free(source);
  module->loading = false;
  csPopTempRoot();
  csPopTempRoot();

  if (body == NULL) return NULL;

  module->body = body;
  vm.pending[vm.pendingCount++] = module;
  return module;
}

/* Resolves and compiles the file and its imports. */
static InterpretResult loadEntryPoint(const char *path) {
  char resolved[PATH_MAX];
  if (realpath(path, resolved) == NULL) {
    fprintf(stderr, "cscript: could not open '%s'\n", path);
    return CS_COMPILE_ERROR;
  }
  if (csModuleLoadResolved(resolved, path, NULL, 0) == NULL) return CS_COMPILE_ERROR;
  return CS_OK;
}

InterpretResult csRunFile(const char *path) {
  InterpretResult loaded = loadEntryPoint(path);
  if (loaded != CS_OK) return loaded;

  InterpretResult result = csVMRunPendingModules();
  if (result != CS_OK) return result;

  /* The program has run; anything it left pending runs now. */
  return csVMRunEventLoop();
}

InterpretResult csCheckFile(const char *path) { return loadEntryPoint(path); }
