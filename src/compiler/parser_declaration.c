/* parser_declaration.c — variable declarations and the patterns they bind.
 *
 * `let`, `const`, and the array and object patterns either can destructure
 * into — nested, with defaults, with a rest element. A pattern is parsed as a
 * pattern rather than reinterpreted from an expression, which is why
 * `[a, b] = xs` and `const [a, b] = xs` take different paths here.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

/* parser_declaration.c — things that introduce a name.
 *
 * Variable declarations and the patterns they may bind, class bodies, and the\n * import and export forms. Grouped because they share one question — what does\n * this bind, and under what name — rather than because they look alike.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"


/* `class Name extends Base { field; field = init; constructor() {} m() {} static m() {} }`
 *
 * Members are separated by nothing at all in JavaScript, so the loop reads one
 * member at a time and decides what it was from the token after the name: a
 * '(' means a method, anything else a field. */

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

  AstNode *list = parseDeclaratorList(parser, line, isConst);
  if (list == NULL) return NULL;

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a variable declaration");
  if (parser->diag->panicMode) return NULL;
  return list;
}

/* `a = 1, b = 2` — everything after `let`, and before the semicolon that the
 * caller owns. Several declarations come back as a statement list, which
 * compiles as one and opens no scope of its own: that is what lets a `for`
 * loop declare two variables and still see both in its condition. */
AstNode *parseDeclaratorList(Parser *parser, int line, bool isConst) {
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

  return list != NULL ? list : first;
}
