/* parser_declaration.c — things that introduce a name.
 *
 * Variable declarations and the patterns they may bind, class bodies, and the\n * import and export forms. Grouped because they share one question — what does\n * this bind, and under what name — rather than because they look alike.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "parser_internal.h"


/* `class Name extends Base { field; field = init; constructor() {} m() {} static m() {} }`
 *
 * Members are separated by nothing at all in JavaScript, so the loop reads one
 * member at a time and decides what it was from the token after the name: a
 * '(' means a method, anything else a field. */
AstNode *parseClass(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_IDENTIFIER, "expected a class name");
  if (parser->diag->panicMode) return NULL;
  const char *name = parser->previous.start;
  int nameLength = parser->previous.length;

  const char *superName = NULL;
  int superLength = 0;
  if (matchToken(parser, TOKEN_EXTENDS)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a superclass name after 'extends'");
    if (parser->diag->panicMode) return NULL;
    superName = parser->previous.start;
    superLength = parser->previous.length;
    if (superLength == nameLength &&
        memcmp(superName, name, (size_t)superLength) == 0) {
      errorAtCurrent(parser, "a class cannot extend itself");
      return NULL;
    }
  }

  AstNode *node =
      csAstClass(parser->arena, line, name, nameLength, superName, superLength);

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' to open the class body");
  if (parser->diag->panicMode) return NULL;

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    /* A stray ';' between members is legal and means nothing. */
    if (matchToken(parser, TOKEN_SEMICOLON)) continue;

    if (check(parser, TOKEN_LEFT_BRACKET)) {
      errorAtCurrent(parser, "computed member names are not supported yet");
      return NULL;
    }

    bool isStatic = matchToken(parser, TOKEN_STATIC);
    if (isStatic && check(parser, TOKEN_LEFT_BRACE)) {
      errorAtCurrent(parser, "static initialisation blocks are not supported yet");
      return NULL;
    }

    /* `async m() {}` — an `async` followed by another name, rather than by
     * `(`, is the modifier and not the method's own name. */
    bool isAsyncMember = false;
    if (checkWord(parser, "async")) {
      Lexer probe = parser->lexer;
      Token next = csLexerNext(&probe);
      if (next.type != TOKEN_LEFT_PAREN && next.type != TOKEN_EQUAL &&
          next.type != TOKEN_SEMICOLON && next.type != TOKEN_COLON) {
        advanceToken(parser);
        isAsyncMember = true;
      }
    }

    if (!consumePropertyName(parser, "expected a member name")) return NULL;
    if (parser->diag->panicMode) return NULL;
    const char *memberName = parser->previous.start;
    int memberLength = parser->previous.length;
    int memberLine = parser->previous.line;

    /* `get x() {}` — an accessor, unless `get` is the member's own name, which
     * it is whenever `(` follows it directly. */
    ClassMemberKind memberKind = MEMBER_METHOD;
    if ((nameIs(memberName, memberLength, "get") ||
         nameIs(memberName, memberLength, "set")) &&
        !check(parser, TOKEN_LEFT_PAREN)) {
      memberKind = memberName[0] == 'g' ? MEMBER_GETTER : MEMBER_SETTER;
      if (!consumePropertyName(parser, "expected a name after 'get' or 'set'")) {
        return NULL;
      }
      memberName = parser->previous.start;
      memberLength = parser->previous.length;
      memberLine = parser->previous.line;
    }

    if (check(parser, TOKEN_LEFT_PAREN)) {
      /* The constructor is named after its class, because that is what an
       * arity error or a stack frame should say: `Dog expects 1 argument`
       * rather than `constructor expects 1 argument`. */
      bool isConstructor = nameIs(memberName, memberLength, "constructor");
      if (isConstructor && isAsyncMember) {
        errorAtCurrent(parser, "a constructor cannot be async");
        return NULL;
      }
      parser->pendingAsync = isAsyncMember;
      AstNode *method = parseFunctionRest(parser, memberLine,
                                          isConstructor ? name : memberName,
                                          isConstructor ? nameLength : memberLength,
                                          true);
      if (method == NULL) return NULL;

      if (isConstructor) {
        if (isStatic) {
          errorAtCurrent(parser, "a constructor cannot be static");
          return NULL;
        }
        if (node->as.classDecl.constructor != NULL) {
          errorAtCurrent(parser, "a class can only have one constructor");
          return NULL;
        }
        node->as.classDecl.constructor = method;
      } else {
        csAstClassAddMember(parser->arena, node, method, isStatic, memberKind);
      }
      continue;
    }

    TypeKind fieldType;
    bool annotated;
    if (!parseTypeAnnotation(parser, &fieldType, &annotated)) return NULL;

    AstNode *initializer = NULL;
    if (matchToken(parser, TOKEN_EQUAL)) {
      initializer = parseExpression(parser);
      if (initializer == NULL) return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the field declaration");
    if (parser->diag->panicMode) return NULL;

    csAstClassAddField(parser->arena, node, memberName, memberLength, initializer,
                       fieldType, annotated, isStatic);
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the class body");
  if (parser->diag->panicMode) return NULL;
  return node;
}

/* The `{ a, b as c }` shared by import and export lists. */
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

  AstNode *literal = makeStringLiteral(parser, parser->previous.start,
                                       parser->previous.length, parser->previous.line);
  if (literal == NULL) return false;

  const char *text = literal->as.string.chars;
  int length = literal->as.string.length;
  bool relative = (length > 2 && text[0] == '.' && text[1] == '/') ||
                  (length > 3 && text[0] == '.' && text[1] == '.' && text[2] == '/');
  if (!relative) {
    csDiagnosticError(parser->diag, parser->previous.line, NULL, 0,
                      "a module path must be relative and start with './' or '../'");
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
  } else {
    errorAtCurrent(parser,
                   "a default import is not supported; write "
                   "'import { name } from \"...\"' or 'import * as ns from \"...\"'");
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
    errorAtCurrent(parser, "'export *' is not supported; name what you re-export");
    return NULL;
  }
  if (check(parser, TOKEN_DEFAULT)) {
    errorAtCurrent(parser,
                   "a default export is not supported; export a named binding "
                   "instead");
    return NULL;
  }

  if (check(parser, TOKEN_LEFT_BRACE)) {
    AstNode *node = csAstExport(parser->arena, line, NULL);
    if (node == NULL) return NULL;
    if (!parseModuleNameList(parser, node, false)) return NULL;
    if (checkContextual(parser, "from")) {
      errorAtCurrent(parser, "re-exporting from another module is not supported yet");
      return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the export list");
    if (parser->diag->panicMode) return NULL;
    return node;
  }

  bool exportsAsyncFunction = checkWord(parser, "async") && nextStartsFunction(parser);
  if (!exportsAsyncFunction && !check(parser, TOKEN_LET) &&
      !check(parser, TOKEN_CONST) && !check(parser, TOKEN_FUNCTION) &&
      !check(parser, TOKEN_CLASS)) {
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
AstNode *finishVarDeclaration(Parser *parser, int line, const char *name,
                                     int nameLength, bool isConst) {
  TypeKind declaredType;
  bool hasAnnotation;
  if (!parseTypeAnnotation(parser, &declaredType, &hasAnnotation)) return NULL;

  AstNode *initializer = NULL;
  if (matchToken(parser, TOKEN_EQUAL)) {
    initializer = parsePrecedence(parser, PREC_ASSIGNMENT);
    if (initializer == NULL) return NULL;
  } else if (isConst) {
    /* A const with no value could never be given one, so it is always a mistake. */
    csDiagnosticError(parser->diag, line, name, nameLength,
                      "'const' declarations must be initialised");
    return NULL;
  }


  /* `const f = () => ...` names the function `f`, the way JavaScript infers a
   * name for an anonymous function assigned straight to a binding. It shows up
   * only in diagnostics and in what console.log prints, which is exactly where
   * an unnamed function is least helpful. */
  if (initializer != NULL && initializer->type == AST_FUNCTION &&
      initializer->as.function.name == NULL) {
    AstNode *named = csAstFunction(parser->arena, initializer->line, name, nameLength);
    if (named != NULL) {
      named->as.function.params = initializer->as.function.params;
      named->as.function.paramCount = initializer->as.function.paramCount;
      named->as.function.body = initializer->as.function.body;
      named->as.function.returnType = initializer->as.function.returnType;
      named->as.function.hasReturnAnnotation =
          initializer->as.function.hasReturnAnnotation;
      named->as.function.nameIsInferred = true;
      initializer = named;
    }
  }

  return csAstVarDecl(parser->arena, line, name, nameLength, initializer, isConst,
                      declaredType, hasAnnotation);
}

/* One pattern — `[a, b]` or `{ x, y: z = 1 }` — with the opening bracket
 * already consumed. Recursive, so a piece of a pattern may be a pattern.
 * Shared by declarations and by parameters, which differ only in what supplies
 * the value. */
AstNode *parsePattern(Parser *parser, bool isObject, bool isConst) {
  int line = parser->previous.line;
  AstNode *pattern = csAstDestructure(parser->arena, line, isObject, isConst);
  TokenType closer = isObject ? TOKEN_RIGHT_BRACE : TOKEN_RIGHT_BRACKET;

  if (!check(parser, closer)) {
    do {
      if (check(parser, closer)) break; /* a trailing comma */

      bool isRest = matchToken(parser, TOKEN_ELLIPSIS);

      /* An array pattern may hold a pattern directly; an object pattern gets
       * to one through a key, as in `{ a: { b } }`. */
      if (!isObject && (check(parser, TOKEN_LEFT_BRACKET) ||
                        check(parser, TOKEN_LEFT_BRACE))) {
        if (isRest) {
          errorAtCurrent(parser, "a rest element must be a plain name");
          return NULL;
        }
        bool nestedIsObject = check(parser, TOKEN_LEFT_BRACE);
        advanceToken(parser);
        AstNode *nested = parsePattern(parser, nestedIsObject, isConst);
        if (nested == NULL) return NULL;
        /* The binding itself names nothing; the nested pattern does the work.
         * The placeholder name is one no source can write. */
        csAstDestructureAdd(parser->arena, pattern, NULL, 0, " nested", 7, NULL, false);
        csAstDestructureNest(pattern, nested);
        continue;
      }

      consume(parser, TOKEN_IDENTIFIER, "expected a name in the pattern");
      if (parser->diag->panicMode) return NULL;

      const char *key = parser->previous.start;
      int keyLength = parser->previous.length;
      const char *name = key;
      int nameLength = keyLength;
      AstNode *nested = NULL;

      /* `{ key: localName }` renames on the way in, and `{ key: [a, b] }`
       * destructures the property further. */
      if (isObject && matchToken(parser, TOKEN_COLON)) {
        if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
          bool nestedIsObject = check(parser, TOKEN_LEFT_BRACE);
          advanceToken(parser);
          nested = parsePattern(parser, nestedIsObject, isConst);
          if (nested == NULL) return NULL;
          name = " nested";
          nameLength = 7;
        } else {
          consume(parser, TOKEN_IDENTIFIER, "expected a name after ':'");
          if (parser->diag->panicMode) return NULL;
          name = parser->previous.start;
          nameLength = parser->previous.length;
        }
      }

      AstNode *defaultValue = NULL;
      if (matchToken(parser, TOKEN_EQUAL)) {
        defaultValue = parsePrecedence(parser, PREC_ASSIGNMENT);
        if (defaultValue == NULL) return NULL;
      }

      csAstDestructureAdd(parser->arena, pattern, isObject ? key : NULL, keyLength,
                          name, nameLength, defaultValue, isRest);
      if (nested != NULL) csAstDestructureNest(pattern, nested);

      if (isRest) break; /* nothing may follow a rest element */
    } while (matchToken(parser, TOKEN_COMMA));
  }

  consume(parser, closer, isObject ? "expected '}' to close the pattern"
                                   : "expected ']' to close the pattern");
  return parser->diag->panicMode ? NULL : pattern;
}

AstNode *parseDestructuring(Parser *parser, bool isObject, bool isConst) {
  AstNode *pattern = parsePattern(parser, isObject, isConst);
  if (pattern == NULL) return NULL;

  consume(parser, TOKEN_EQUAL, "a destructuring declaration needs an initialiser");
  if (parser->diag->panicMode) return NULL;

  pattern->as.destructure.initializer = parsePrecedence(parser, PREC_ASSIGNMENT);
  if (pattern->as.destructure.initializer == NULL) return NULL;

  consume(parser, TOKEN_SEMICOLON, "expected ';' after the declaration");
  return parser->diag->panicMode ? NULL : pattern;
}

/* `let a = 1, b = 2;` — one keyword, several bindings.
 *
 * The result is an AST_PROGRAM rather than an AST_BLOCK when there is more
 * than one, because a block would put them in a scope of their own and they
 * belong to the enclosing one. */
AstNode *parseVarDeclaration(Parser *parser, bool isConst) {
  int line = parser->previous.line;

  /* A pattern rather than a name means this is a destructuring declaration. */
  if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
    return parseDestructuring(parser, false, isConst);
  }
  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    return parseDestructuring(parser, true, isConst);
  }

  AstNode *first = NULL;
  AstNode *list = NULL;

  for (;;) {
    consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
    if (parser->diag->panicMode) return NULL;

    AstNode *declaration = finishVarDeclaration(parser, line, parser->previous.start,
                                                parser->previous.length, isConst);
    if (declaration == NULL) return NULL;

    if (first == NULL) {
      first = declaration;
    } else {
      if (list == NULL) {
        list = csAstProgram(parser->arena, line);
        csAstProgramAdd(parser->arena, list, first);
      }
      csAstProgramAdd(parser->arena, list, declaration);
    }

    if (!matchToken(parser, TOKEN_COMMA)) break;
  }

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a variable declaration");
  if (parser->diag->panicMode) return NULL;
  return list != NULL ? list : first;
}
