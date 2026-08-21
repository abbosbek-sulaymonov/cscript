/* compiler_module.c — imports and exports.
 *
 * Both are resolved at compile time: the loader has already compiled every\n * module this one imports, so a missing export is a compile error and an\n * import is a constant reference to a module that already exists.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler/compiler_internal.h"


/* Binds one name at module top level. Imports and the classes and functions
 * around them all land in the module's own global table. */
void defineModuleBinding(const char *name, int length, int line) {
  addGlobal(name, length, true, line);
  emitConstantOp(OP_DEFINE_CONST, identifierConstant(name, length, line), line);
}

void compileImport(const AstNode *node) {
  int line = node->line;

  if (current->kind != FUNCTION_SCRIPT || current->scopeDepth > 0) {
    errorAt(line, "'import' is only allowed at the top level of a file");
    return;
  }

  /* The loader compiled this module's dependencies before it reached here, so
   * the module being named is already registered — and its export list is
   * known, which is what turns a missing export into a compile error. */
  char resolved[4096];
  if (!csModuleResolve(currentUnit->module->path->chars, node->as.import.specifier,
                       resolved, sizeof resolved)) {
    errorAt(line, "cannot find module '%s'", node->as.import.specifier);
    return;
  }

  ObjModule *imported = csModuleFind(resolved);
  if (imported == NULL) {
    errorAt(line, "module '%s' was not loaded", node->as.import.specifier);
    return;
  }

  int moduleConstant = makeConstant(OBJ_VAL(imported), line);

  if (node->as.import.namespaceName != NULL) {
    emitConstantOp(OP_CONSTANT, moduleConstant, line);
    emitByte(OP_IMPORT_NAMESPACE, line);
    defineModuleBinding(node->as.import.namespaceName, node->as.import.namespaceLength,
                        line);
  }

  if (node->as.import.defaultName != NULL) {
    ObjString *key = csStringCopy("default", 7);
    if (!csTableGet(&imported->exports, key, NULL)) {
      errorAt(line, "'%s' has no default export", node->as.import.specifier);
    } else {
      emitConstantOp(OP_CONSTANT, moduleConstant, line);
      emitConstantOp(OP_IMPORT_NAME, identifierConstant("default", 7, line), line);
      defineModuleBinding(node->as.import.defaultName, node->as.import.defaultLength,
                          line);
    }
  }

  for (int i = 0; i < node->as.import.nameCount; i++) {
    const AstModuleName *entry = &node->as.import.names[i];
    ObjString *exported = csStringCopy(entry->name, entry->nameLength);
    if (!csTableGet(&imported->exports, exported, NULL)) {
      errorAt(line, "'%s' has no export named '%.*s'", node->as.import.specifier,
              entry->nameLength, entry->name);
      continue;
    }

    emitConstantOp(OP_CONSTANT, moduleConstant, line);
    emitConstantOp(OP_IMPORT_NAME,
                   identifierConstant(entry->name, entry->nameLength, line), line);
    defineModuleBinding(entry->alias, entry->aliasLength, line);
  }
}

/* Records a name as exported. The export table is a set: an import reads the
 * value out of the module's globals when it runs, so the two never disagree
 * and a binding stays live. */
void markExported(const char *name, int length, int line) {
  ObjString *key = csStringCopy(name, length);
  csPushTempRoot((Obj *)key);
  csTableSet(&currentUnit->module->exports, key, BOOL_VAL(true));
  csPopTempRoot();
  (void)line;
}

void compileExport(const AstNode *node) {
  int line = node->line;

  if (current->kind != FUNCTION_SCRIPT || current->scopeDepth > 0) {
    errorAt(line, "'export' is only allowed at the top level of a file");
    return;
  }

  /* `export … from "./m.cx"`. The module is already compiled, so its export
   * list is known here and a name that is not in it is a compile error — the
   * same guarantee a plain import gets. */
  if (node->as.export.specifier != NULL) {
    char resolved[4096];
    if (!csModuleResolve(currentUnit->module->path->chars, node->as.export.specifier,
                         resolved, sizeof resolved)) {
      errorAt(line, "cannot find module '%s'", node->as.export.specifier);
      return;
    }
    ObjModule *from = csModuleFind(resolved);
    if (from == NULL) {
      errorAt(line, "module '%s' was not loaded", node->as.export.specifier);
      return;
    }
    int moduleConstant = makeConstant(OBJ_VAL(from), line);

    if (node->as.export.isStar) {
      /* Everything it exports except its default, which `export *` leaves
       * behind — two modules re-exported into one would otherwise each claim
       * the same name. */
      for (int i = 0; i < from->exports.capacity; i++) {
        ObjString *name = from->exports.entries[i].key;
        if (name == NULL) continue;
        if (name->length == 7 && memcmp(name->chars, "default", 7) == 0) continue;
        /* A name this file already has stays as it is. JavaScript lets an
         * explicit export win over a star, which is what makes
         * `export { a } from "…"; export * from "…";` mean something. */
        if (findGlobal(name->chars, name->length) != NULL) continue;

        emitConstantOp(OP_CONSTANT, moduleConstant, line);
        emitConstantOp(OP_IMPORT_NAME,
                       identifierConstant(name->chars, name->length, line), line);
        defineModuleBinding(name->chars, name->length, line);
        markExported(name->chars, name->length, line);
      }
      return;
    }

    for (int i = 0; i < node->as.export.nameCount; i++) {
      const AstModuleName *entry = &node->as.export.names[i];
      ObjString *exported = csStringCopy(entry->name, entry->nameLength);
      if (!csTableGet(&from->exports, exported, NULL)) {
        errorAt(line, "'%s' has no export named '%.*s'", node->as.export.specifier,
                entry->nameLength, entry->name);
        continue;
      }
      emitConstantOp(OP_CONSTANT, moduleConstant, line);
      emitConstantOp(OP_IMPORT_NAME,
                     identifierConstant(entry->name, entry->nameLength, line), line);
      defineModuleBinding(entry->alias, entry->aliasLength, line);
      markExported(entry->alias, entry->aliasLength, line);
    }
    return;
  }

  /* `export default …`. Filed under a name that is a keyword, so the only way
   * back to it is an import. */
  if (node->as.export.isDefault) {
    const AstNode *declaration = node->as.export.declaration;

    if (declaration->type == AST_FUNCTION || declaration->type == AST_CLASS_DECL) {
      /* `export default function f() {}` declares `f` here too, so the
       * declaration is compiled once and its binding loaded back. */
      compileNode(declaration);
      if (declaration->type == AST_FUNCTION) {
        compileIdentifierLoad(declaration->as.function.name,
                              declaration->as.function.nameLength, line);
      } else {
        compileIdentifierLoad(declaration->as.classDecl.name,
                              declaration->as.classDecl.nameLength, line);
      }
    } else {
      /* An expression. Only its value is wanted, so the statement wrapper —
       * whose whole job is to discard it — is stepped over rather than
       * compiled and then compensated for. */
      compileNode(declaration->as.expression);
    }

    defineModuleBinding("default", 7, line);
    markExported("default", 7, line);
    return;
  }

  if (node->as.export.declaration == NULL) {
    /* `export { a, b as c };` — the names must already be declared here. */
    for (int i = 0; i < node->as.export.nameCount; i++) {
      const AstModuleName *entry = &node->as.export.names[i];
      if (findGlobal(entry->name, entry->nameLength) == NULL) {
        errorAt(line, "'%.*s' is not declared in this file", entry->nameLength,
                entry->name);
        continue;
      }
      /* An alias exports the binding under a different name, so the export
       * table records the alias and the import reads it back through it. */
      markExported(entry->alias, entry->aliasLength, line);
      if (entry->aliasLength != entry->nameLength ||
          memcmp(entry->alias, entry->name, (size_t)entry->nameLength) != 0) {
        /* Bind the alias to the same value so the read has somewhere to go. */
        compileIdentifierLoad(entry->name, entry->nameLength, line);
        addGlobal(entry->alias, entry->aliasLength, true, line);
        emitConstantOp(OP_DEFINE_CONST,
                       identifierConstant(entry->alias, entry->aliasLength, line), line);
      }
    }
    return;
  }

  const AstNode *declaration = node->as.export.declaration;
  compileNode(declaration);

  switch (declaration->type) {
    case AST_VAR_DECL:
      markExported(declaration->as.varDecl.name, declaration->as.varDecl.length, line);
      break;
    case AST_FUNCTION:
      markExported(declaration->as.function.name, declaration->as.function.nameLength,
                   line);
      break;
    case AST_CLASS_DECL:
      markExported(declaration->as.classDecl.name, declaration->as.classDecl.nameLength,
                   line);
      break;
    case AST_DESTRUCTURE:
      for (int i = 0; i < declaration->as.destructure.count; i++) {
        const AstBinding *binding = &declaration->as.destructure.bindings[i];
        markExported(binding->name, binding->nameLength, line);
      }
      break;
    default:
      errorAt(line, "this declaration cannot be exported");
      break;
  }
}
