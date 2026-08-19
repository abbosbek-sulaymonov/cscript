#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cscript/ast.h"
#include "cscript/compiler.h"
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

bool csModuleResolve(const char *fromPath, const char *specifier, char *out,
                     size_t outSize) {
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
      csDiagnosticError(diag, statement->line, NULL, 0, "cannot find module '%s'",
                        statement->as.import.specifier);
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

  AstNode *program = csParse(source, &arena, &diag);
  bool ok = program != NULL;
  if (ok) ok = csModuleLoadImports(program, resolvedPath, &diag);
  if (ok) ok = csTypeCheck(program, &diag);

  ObjFunction *body = ok ? csCompile(program, module, &diag) : NULL;

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

InterpretResult csRunFile(const char *path) {
  char resolved[PATH_MAX];
  if (realpath(path, resolved) == NULL) {
    fprintf(stderr, "cscript: could not open '%s'\n", path);
    return CS_COMPILE_ERROR;
  }

  if (csModuleLoadResolved(resolved, path, NULL, 0) == NULL) return CS_COMPILE_ERROR;

  InterpretResult result = csVMRunPendingModules();
  if (result != CS_OK) return result;

  /* The program has run; anything it left pending runs now. */
  return csVMRunEventLoop();
}
