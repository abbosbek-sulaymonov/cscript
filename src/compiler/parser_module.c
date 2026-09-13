/* parser_module.c — `import` and `export`, in every form they take.
 *
 * Named, default, namespace, `export *`, and re-exports. Both sides share the
 * name-list and the specifier parsing, because `export { a as b }` and
 * `import { a as b }` are the same list with the arrow pointing the other
 * way.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

bool parseModuleNameList(Parser *parser, AstNode *node, bool isImport) {
  consume(parser, TOKEN_LEFT_BRACE, "expected '{' after the list");
  if (parser->diag->panicMode) return false;

  if (!check(parser, TOKEN_RIGHT_BRACE)) {
    do {
      /* A trailing comma before the brace is legal. */
      if (check(parser, TOKEN_RIGHT_BRACE)) break;

      consume(parser, TOKEN_IDENTIFIER, "expected a name");
      if (parser->diag->panicMode) return false;
      const char *name = parser->previous.start;
      int nameLength = parser->previous.length;

      const char *alias = name;
      int aliasLength = nameLength;
      if (matchContextual(parser, "as")) {
        consume(parser, TOKEN_IDENTIFIER, "expected a name after 'as'");
        if (parser->diag->panicMode) return false;
        alias = parser->previous.start;
        aliasLength = parser->previous.length;
      }

      if (isImport) {
        csAstImportAddName(parser->arena, node, name, nameLength, alias, aliasLength);
      } else {
        csAstExportAddName(parser->arena, node, name, nameLength, alias, aliasLength);
      }
    } while (matchToken(parser, TOKEN_COMMA));
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' to close the list");
  return !parser->diag->panicMode;
}

/* The `from "./path.cx"` tail, whose specifier has to name a file this program
 * can actually find: a relative path, extension written out. There is no
 * package system to resolve a bare name against, and guessing extensions is
 * how a module system starts needing a resolver nobody can predict. */
bool parseModuleSpecifier(Parser *parser, const char **out, int *outLength) {
  if (!matchContextual(parser, "from")) {
    errorAtCurrent(parser, "expected 'from' after the imported names");
    return false;
  }
  consume(parser, TOKEN_STRING, "expected a quoted module path after 'from'");
  if (parser->diag->panicMode) return false;

  AstNode *literal = makeStringLiteral(parser, parser->previous.start, parser->previous.length, parser->previous.line);
  if (literal == NULL) return false;

  const char *text = literal->as.string.chars;
  int length = literal->as.string.length;
  bool relative = (length > 2 && text[0] == '.' && text[1] == '/') || (length > 3 && text[0] == '.' && text[1] == '.' && text[2] == '/');
  /* The one specifier that is not a path: `std:iter` names a module in the
   * library that ships with the language, wherever that has been installed.
   * Everything else has to be relative, so that reading an import tells you
   * which file it means without knowing about a search path. */
  bool standard = length > 4 && strncmp(text, "std:", 4) == 0;
  if (!relative && !standard) {
    csDiagnosticError(parser->diag, parser->previous.line, NULL, 0,
                      "a module path must be relative and start with './' or "
                      "'../', or name a standard-library module as 'std:name'");
    return false;
  }

  *out = text;
  *outLength = length;
  return true;
}

/* `import { a, b as c } from "./m.cx";` and `import * as ns from "./m.cx";` */
AstNode *parseImport(Parser *parser) {
  int line = parser->previous.line;

  const char *namespaceName = NULL;
  int namespaceLength = 0;
  AstNode *node = NULL;

  if (matchToken(parser, TOKEN_STAR)) {
    if (!matchContextual(parser, "as")) {
      errorAtCurrent(parser, "expected 'as' after 'import *'");
      return NULL;
    }
    consume(parser, TOKEN_IDENTIFIER, "expected a name after 'as'");
    if (parser->diag->panicMode) return NULL;
    namespaceName = parser->previous.start;
    namespaceLength = parser->previous.length;

    const char *specifier;
    int specifierLength;
    if (!parseModuleSpecifier(parser, &specifier, &specifierLength)) return NULL;
    node = csAstImport(parser->arena, line, specifier, specifierLength);
    if (node == NULL) return NULL;
    node->as.import.namespaceName = namespaceName;
    node->as.import.namespaceLength = namespaceLength;
  } else if (check(parser, TOKEN_LEFT_BRACE)) {
    node = csAstImport(parser->arena, line, "", 0);
    if (node == NULL) return NULL;
    if (!parseModuleNameList(parser, node, true)) return NULL;

    const char *specifier;
    int specifierLength;
    if (!parseModuleSpecifier(parser, &specifier, &specifierLength)) return NULL;
    node->as.import.specifier = specifier;
    node->as.import.specifierLength = specifierLength;
  } else if (check(parser, TOKEN_IDENTIFIER)) {
    /* `import d from "./m.cx";` and `import d, { a } from "./m.cx";`
     *
     * A default import is a named import of the name `default`, which is a
     * keyword and so cannot be written any other way. */
    advanceToken(parser);
    const char *defaultName = parser->previous.start;
    int defaultLength = parser->previous.length;

    node = csAstImport(parser->arena, line, "", 0);
    if (node == NULL) return NULL;
    node->as.import.defaultName = defaultName;
    node->as.import.defaultLength = defaultLength;

    if (matchToken(parser, TOKEN_COMMA)) {
      if (matchToken(parser, TOKEN_STAR)) {
        if (!matchContextual(parser, "as")) {
          errorAtCurrent(parser, "expected 'as' after 'import *'");
          return NULL;
        }
        consume(parser, TOKEN_IDENTIFIER, "expected a name after 'as'");
        if (parser->diag->panicMode) return NULL;
        node->as.import.namespaceName = parser->previous.start;
        node->as.import.namespaceLength = parser->previous.length;
      } else if (!parseModuleNameList(parser, node, true)) {
        return NULL;
      }
    }

    const char *specifier;
    int specifierLength;
    if (!parseModuleSpecifier(parser, &specifier, &specifierLength)) return NULL;
    node->as.import.specifier = specifier;
    node->as.import.specifierLength = specifierLength;
  } else {
    errorAtCurrent(parser, "expected a name, '{' or '* as' after 'import'");
    return NULL;
  }

  consume(parser, TOKEN_SEMICOLON, "expected ';' after the import");
  if (parser->diag->panicMode) return NULL;
  return node;
}

/* `export const x = 1;`, `export function f() {}`, `export class C {}` and
 * `export { a, b as c };` */
AstNode *parseExport(Parser *parser) {
  int line = parser->previous.line;

  if (matchToken(parser, TOKEN_STAR)) {
    /* `export * from "./m.cx";` — everything that module exports, except its
     * default, which JavaScript deliberately leaves behind. */
    AstNode *node = csAstExport(parser->arena, line, NULL);
    if (node == NULL) return NULL;
    node->as.export.isStar = true;

    if (!checkContextual(parser, "from")) {
      errorAtCurrent(parser, "expected 'from' after 'export *'");
      return NULL;
    }
    if (!parseModuleSpecifier(parser, &node->as.export.specifier, &node->as.export.specifierLength)) {
      return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the export");
    if (parser->diag->panicMode) return NULL;
    return node;
  }

  if (matchToken(parser, TOKEN_DEFAULT)) {
    /* `export default …`. The binding is filed under `default`, a keyword no
     * source can name, so it is reachable only through an import. */
    AstNode *node = csAstExport(parser->arena, line, NULL);
    if (node == NULL) return NULL;
    node->as.export.isDefault = true;

    if (matchToken(parser, TOKEN_CLASS)) {
      node->as.export.declaration = parseClass(parser);
    } else if (matchToken(parser, TOKEN_FUNCTION)) {
      node->as.export.declaration = parseFunction(parser, true);
    } else {
      AstNode *value = parseExpression(parser);
      if (value == NULL) return NULL;
      node->as.export.declaration = csAstExpressionStmt(parser->arena, line, value);
      consume(parser, TOKEN_SEMICOLON, "expected ';' after the exported value");
    }
    if (node->as.export.declaration == NULL || parser->diag->panicMode) return NULL;
    return node;
  }

  if (check(parser, TOKEN_LEFT_BRACE)) {
    AstNode *node = csAstExport(parser->arena, line, NULL);
    if (node == NULL) return NULL;
    if (!parseModuleNameList(parser, node, false)) return NULL;
    /* `export { a, b as c } from "./m.cx";` — the names come from there and
     * are never bound here under their own names. */
    if (checkContextual(parser, "from")) {
      if (!parseModuleSpecifier(parser, &node->as.export.specifier, &node->as.export.specifierLength)) {
        return NULL;
      }
      consume(parser, TOKEN_SEMICOLON, "expected ';' after the export list");
      if (parser->diag->panicMode) return NULL;
      return node;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the export list");
    if (parser->diag->panicMode) return NULL;
    return node;
  }

  /* An interface is erased, and types do not cross a module boundary yet, so
   * exporting one would export nothing. Saying so beats "expected a
   * declaration", which reads as though the syntax were wrong. */
  if (startsInterfaceDeclaration(parser) || startsTypeAlias(parser)) {
    errorAtCurrent(parser, "a type is file-local: types do not cross a module boundary yet, so there is nothing to export");
    return NULL;
  }

  bool exportsAsyncFunction = checkWord(parser, "async") && nextStartsFunction(parser);
  if (!exportsAsyncFunction && !check(parser, TOKEN_LET) && !check(parser, TOKEN_CONST) && !check(parser, TOKEN_FUNCTION) && !check(parser, TOKEN_CLASS)) {
    errorAtCurrent(parser, "'export' must be followed by a declaration or '{'");
    return NULL;
  }

  AstNode *declaration = parseStatement(parser);
  if (declaration == NULL) return NULL;
  return csAstExport(parser->arena, line, declaration);
}

/* Parses everything after a binding's name: the optional annotation, the
 * optional initialiser and the semicolon.
 *
 * Split out from parseVarDeclaration because `for (const x of xs)` and
 * `for (let i = 0; ...)` only diverge after the name, so the caller has to read
 * it before it knows which form it is looking at. */
AstNode *finishVarDeclaration(Parser *parser, int line, const char *name, int nameLength, bool isConst) {
  TypeKind declaredType;
  bool hasAnnotation;
  if (!parseTypeAnnotation(parser, &declaredType, &hasAnnotation)) return NULL;

  AstNode *initializer = NULL;
  if (matchToken(parser, TOKEN_EQUAL)) {
    initializer = parsePrecedence(parser, PREC_ASSIGNMENT);
    if (initializer == NULL) return NULL;
  } else if (isConst) {
    /* A const with no value could never be given one, so it is always a mistake. */
    csDiagnosticError(parser->diag, line, name, nameLength, "'const' declarations must be initialised");
    return NULL;
  }

  /* `const f = () => ...` names the function `f`, the way JavaScript infers a
   * name for an anonymous function assigned straight to a binding. It shows up
   * only in diagnostics and in what console.log prints, which is exactly where
   * an unnamed function is least helpful. */
  if (initializer != NULL && initializer->type == AST_FUNCTION && initializer->as.function.name == NULL) {
    AstNode *named = csAstFunction(parser->arena, initializer->line, name, nameLength);
    if (named != NULL) {
      /* Everything the function already was, and then the name.
       *
       * This used to copy the fields it knew about one by one, which meant
       * every field added afterwards was silently dropped — `async`, `*` and
       * a rest parameter all stopped working the moment a function was
       * assigned to a binding rather than declared. Copying the node and
       * putting the name back cannot go out of date. */
      const char *inferred = named->as.function.name;
      int inferredLength = named->as.function.nameLength;

      *named = *initializer;
      named->as.function.name = inferred;
      named->as.function.nameLength = inferredLength;
      named->as.function.nameIsInferred = true;
      initializer = named;
    }
  }

  return csAstVarDecl(parser->arena, line, name, nameLength, initializer, isConst, declaredType, hasAnnotation);
}

/* One pattern — `[a, b]` or `{ x, y: z = 1 }` — with the opening bracket
 * already consumed. Recursive, so a piece of a pattern may be a pattern.
 * Shared by declarations and by parameters, which differ only in what supplies
 * the value. */
