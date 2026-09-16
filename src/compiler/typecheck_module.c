/* typecheck_module.c — the types that cross a file boundary.
 *
 * A module's types are settled before the file that imports it is checked at
 * all: the loader reads dependencies first, parses and checks each one, and
 * hands the table it built to the module object — which outlives the arena for
 * exactly this reason. So importing a type is not inference or a second pass,
 * it is re-interning a type from one table into another, and that works
 * because everything here is structural: two files describing the same shape
 * describe the same type without either knowing the other.
 *
 * What does not cross: a generic's type variables, erased on the way because
 * an imported binding has no call site the checker can read; and anything from
 * a module still loading, which is what a cycle looks like from here.
 */
#include <limits.h>
#include <string.h>

#include "cscript/module.h"
#include "compiler/typecheck_internal.h"

/* The type a module gives the name being imported, in this file's table. */
TypeId csTypeImportedBinding(Checker *checker, const AstNode *node, const char *name, int length) {
  if (checker->path == NULL || checker->types == NULL) return TYPE_DYNAMIC;

  char resolved[PATH_MAX];
  if (!csModuleResolve(checker->path, node->as.import.specifier, resolved, sizeof resolved)) return TYPE_DYNAMIC;

  ObjModule *module = csModuleFind(resolved);
  if (module == NULL) return TYPE_DYNAMIC;

  const TypeTable *theirs = NULL;
  TypeId type = TYPE_DYNAMIC;
  if (!csModuleExportType(module, name, length, &theirs, &type)) return TYPE_DYNAMIC;
  return csTypeImport(checker->types, theirs, type);
}

/* Records what an `export` gives away, so that whatever imports this file can
 * ask. A re-export — `export { x } from "./m.cx"` — binds nothing here, and
 * what it passes along is already described by the module it came from. */
void csTypeRecordExports(Checker *checker, const AstNode *node) {
  if (checker->program == NULL || node->as.export.specifier != NULL || node->as.export.isStar) return;

  /* `export { a, b };` — each name is a binding this file already has. */
  for (int i = 0; i < node->as.export.nameCount; i++) {
    const AstModuleName *entry = &node->as.export.names[i];
    const Variable *variable = csTypeFindVariable(checker, entry->name, entry->nameLength);
    TypeId type = variable != NULL ? variable->type : TYPE_DYNAMIC;
    csAstProgramAddExport(checker->program, entry->alias, entry->aliasLength, type);
  }

  const AstNode *declaration = node->as.export.declaration;
  if (declaration == NULL) return;

  /* `export default …` is exported under that name, which is what an
   * importing file writes it as. */
  if (node->as.export.isDefault) {
    csAstProgramAddExport(checker->program, "default", 7, declaration->resolvedType);
    return;
  }

  /* `export function f() {}`, `export const x = 1;`, `export class C {}` —
   * the declaration names itself, and the binding it made carries the type. */
  const char *name = NULL;
  int length = 0;
  switch (declaration->type) {
    case AST_FUNCTION:
      name = declaration->as.function.name;
      length = declaration->as.function.nameLength;
      break;
    case AST_VAR_DECL:
      name = declaration->as.varDecl.name;
      length = declaration->as.varDecl.length;
      break;
    case AST_CLASS_DECL:
      name = declaration->as.classDecl.name;
      length = declaration->as.classDecl.nameLength;
      break;
    default: return;
  }
  if (name == NULL) return;

  const Variable *variable = csTypeFindVariable(checker, name, length);
  csAstProgramAddExport(checker->program, name, length, variable != NULL ? variable->type : TYPE_DYNAMIC);
}
