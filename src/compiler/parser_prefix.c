/* parser_prefix.c — the operators an expression can start with.
 *
 * Unary and prefix forms. Each parses its operand at PREC_UNARY, because they
 * bind tighter than any binary operator and are right-associative — and the
 * two that are not operators at all, a class expression and a regex literal,
 * are here because they are what is left once every operand form has said no.
 *
 * Answers NULL without consuming anything for a token that is not one of its
 * own, so parsePrimary asks each group in turn — which is not the same as
 * failing: a malformed expression it *did* recognise has already reported the
 * error, and answers NULL too. Which of the two happened is what the parser's
 * error count says, and parsePrimary is the one place that has to ask.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

AstNode *parsePrefixPrimary(Parser *parser, int line) {
  /* Unary operators bind tighter than any binary operator and are
   * right-associative, so the operand is parsed at PREC_UNARY. */
  if (matchToken(parser, TOKEN_MINUS)) {
    AstNode *operand = parsePrecedence(parser, PREC_UNARY);
    if (operand == NULL) return NULL;
    return csAstUnary(parser->arena, line, UNARY_NEGATE, operand);
  }
  if (matchToken(parser, TOKEN_BANG)) {
    AstNode *operand = parsePrecedence(parser, PREC_UNARY);
    if (operand == NULL) return NULL;
    return csAstUnary(parser->arena, line, UNARY_NOT, operand);
  }
  if (matchToken(parser, TOKEN_TYPEOF)) {
    AstNode *operand = parsePrecedence(parser, PREC_UNARY);
    if (operand == NULL) return NULL;
    return csAstUnary(parser->arena, line, UNARY_TYPEOF, operand);
  }

  if (matchToken(parser, TOKEN_VOID)) {
    AstNode *operand = parsePrecedence(parser, PREC_UNARY);
    if (operand == NULL) return NULL;
    return csAstUnary(parser->arena, line, UNARY_VOID, operand);
  }

  if (matchToken(parser, TOKEN_CLASS)) {
    /* `const C = class { … }` — a class as a value. It binds nothing in the
     * enclosing scope; whatever it is assigned to does. A name may still be
     * written, and is what the class calls itself. */
    const char *name = NULL;
    int nameLength = 0;
    if (check(parser, TOKEN_IDENTIFIER)) {
      advanceToken(parser);
      name = parser->previous.start;
      nameLength = parser->previous.length;
    }

    AstNode *klass = parseClassBody(parser, line, name, nameLength);
    if (klass == NULL) return NULL;
    klass->as.classDecl.isExpression = true;
    return parseCallSuffixes(parser, klass);
  }

  if (matchToken(parser, TOKEN_YIELD)) {
    if (!parser->inGenerator) {
      csDiagnosticError(parser->diag, line, NULL, 0,
                        "'yield' is only allowed inside a generator, written "
                        "'function* name()'");
      return NULL;
    }
    bool isDelegate = matchToken(parser, TOKEN_STAR);

    /* `yield;` and `yield}` produce undefined. Anything that could start an
     * expression is the value. */
    AstNode *value = NULL;
    if (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_RIGHT_BRACE) &&
        !check(parser, TOKEN_RIGHT_PAREN) && !check(parser, TOKEN_RIGHT_BRACKET) &&
        !check(parser, TOKEN_COMMA) && !check(parser, TOKEN_EOF)) {
      value = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (value == NULL) return NULL;
    } else if (isDelegate) {
      errorAtCurrent(parser, "'yield*' needs something to delegate to");
      return NULL;
    }
    return csAstYield(parser->arena, line, value, isDelegate);
  }

  if (matchToken(parser, TOKEN_AWAIT)) {
    if (parser->asyncDepth == 0) {
      csDiagnosticError(parser->diag, line, NULL, 0,
                        "'await' is only allowed inside an async function");
      return NULL;
    }
    AstNode *operand = parsePrecedence(parser, PREC_UNARY);
    if (operand == NULL) return NULL;
    return parseCallSuffixes(parser, csAstAwait(parser->arena, line, operand));
  }

  /* Prefix ++/--, which yields the value from *after* the update. */
  if (check(parser, TOKEN_PLUS_PLUS) || check(parser, TOKEN_MINUS_MINUS)) {
    bool isIncrement = check(parser, TOKEN_PLUS_PLUS);
    advanceToken(parser);
    AstNode *target = parsePrecedence(parser, PREC_UNARY);
    if (target == NULL) return NULL;
    if (target->type != AST_IDENTIFIER) {
      csDiagnosticError(parser->diag, line, NULL, 0, "'%s' needs a variable to update",
                        isIncrement ? "++" : "--");
      return NULL;
    }
    return csAstUpdate(parser->arena, line, target, isIncrement, true);
  }

  if (matchToken(parser, TOKEN_PLUS)) {
    errorAtCurrent(parser, "unary '+' is not supported; write Number(x) instead");
    return NULL;
  }

  if (matchToken(parser, TOKEN_DELETE)) {
    Token keyword = parser->previous;
    AstNode *target = parsePrecedence(parser, PREC_UNARY);
    if (target == NULL) return NULL;

    /* JavaScript answers `true` for `delete x` on anything that is not a
     * property, and rejects a bare variable outright in strict mode. Naming
     * it here is more use than either. */
    if (target->type != AST_PROPERTY && target->type != AST_INDEX) {
      csDiagnosticError(parser->diag, keyword.line, keyword.start, keyword.length,
                        "'delete' removes a property, as in 'delete o.k' or "
                        "'delete o[k]'");
      return NULL;
    }
    return csAstDelete(parser->arena, keyword.line, target);
  }

  /* `/` here can only open a regular expression: a value is expected, so it
   * cannot be division. That is the whole disambiguation, and it lives at the
   * one place that knows. */
  if (check(parser, TOKEN_SLASH)) {
    Token literal = csLexerScanRegex(&parser->lexer);
    if (literal.type != TOKEN_REGEX) return NULL;
    parser->current = literal;
    advanceToken(parser);

    /* The token spans /pattern/flags; the pattern is what lies between the
     * slashes, and the flags are what follows the last one. */
    const char *text = literal.start;
    int closing = literal.length - 1;
    while (closing > 0 && text[closing] != '/') closing--;

    return parseCallSuffixes(
        parser, csAstRegex(parser->arena, literal.line, text + 1, closing - 1,
                           text + closing + 1, literal.length - closing - 1));
  }

  return NULL;
}
