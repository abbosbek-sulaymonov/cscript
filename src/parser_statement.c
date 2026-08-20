/* parser_statement.c — statements and control flow.
 *
 * The statement dispatcher and every construct it recognises: blocks,\n * conditionals, the four loop forms, switch and try. Declarations are next\n * door in parser_declaration.c; this file is what surrounds them.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "parser_internal.h"


/* `try { } catch (e) { } finally { }`
 *
 * At least one of catch and finally must be present, since `try` alone does
 * nothing. The catch binding is optional, matching modern JavaScript. */
AstNode *parseTry(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' after 'try'");
  if (parser->diag->panicMode) return NULL;
  AstNode *body = parseBlock(parser);
  if (body == NULL) return NULL;

  const char *catchName = NULL;
  int catchNameLength = 0;
  AstNode *catchBody = NULL;

  if (matchToken(parser, TOKEN_CATCH)) {
    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      consume(parser, TOKEN_IDENTIFIER, "expected a name for the caught value");
      if (parser->diag->panicMode) return NULL;
      catchName = parser->previous.start;
      catchNameLength = parser->previous.length;
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the catch binding");
      if (parser->diag->panicMode) return NULL;
    }

    consume(parser, TOKEN_LEFT_BRACE, "expected '{' after 'catch'");
    if (parser->diag->panicMode) return NULL;
    catchBody = parseBlock(parser);
    if (catchBody == NULL) return NULL;
  }

  AstNode *finallyBody = NULL;
  if (matchToken(parser, TOKEN_FINALLY)) {
    consume(parser, TOKEN_LEFT_BRACE, "expected '{' after 'finally'");
    if (parser->diag->panicMode) return NULL;
    finallyBody = parseBlock(parser);
    if (finallyBody == NULL) return NULL;
  }

  if (catchBody == NULL && finallyBody == NULL) {
    csDiagnosticError(parser->diag, line, NULL, 0,
                      "a 'try' needs a 'catch' or a 'finally'");
    return NULL;
  }

  return csAstTry(parser->arena, line, body, catchName, catchNameLength, catchBody,
                  finallyBody);
}

/* `switch (subject) { case a: ... default: ... }`
 *
 * Arms are matched with `===`, so there is no coercion here either. Cases do
 * not fall through: each arm ends where the next begins, which removes the
 * single most common switch bug without changing how one is written. */
AstNode *parseSwitch(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'switch'");
  AstNode *subject = parseExpression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the switch subject");
  consume(parser, TOKEN_LEFT_BRACE, "expected '{' to open the switch body");
  if (subject == NULL || parser->diag->panicMode) return NULL;

  AstNode *node = csAstSwitch(parser->arena, line, subject);

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    bool isDefault = matchToken(parser, TOKEN_DEFAULT);
    AstNode *test = NULL;

    if (!isDefault) {
      consume(parser, TOKEN_CASE, "expected 'case' or 'default'");
      if (parser->diag->panicMode) return NULL;
      test = parseExpression(parser);
      if (test == NULL) return NULL;
    }

    consume(parser, TOKEN_COLON, "expected ':' after the case label");
    if (parser->diag->panicMode) return NULL;

    /* Statements run until the next label or the closing brace. */
    AstNode *body = csAstBlock(parser->arena, parser->current.line);
    while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
           !check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
      AstNode *statement = parseStatement(parser);
      if (statement != NULL) csAstProgramAdd(parser->arena, body, statement);
      if (parser->diag->panicMode) synchronize(parser);
    }

    if (isDefault) {
      if (node->as.switchStmt.defaultBody != NULL) {
        csDiagnosticError(parser->diag, line, NULL, 0,
                          "a switch can only have one 'default'");
        return NULL;
      }
      node->as.switchStmt.defaultBody = body;
    } else {
      csAstSwitchAddCase(parser->arena, node, test, body);
    }
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' to close the switch");
  return parser->diag->panicMode ? NULL : node;
}

AstNode *parseBlock(Parser *parser) {
  int line = parser->previous.line;
  AstNode *block = csAstBlock(parser->arena, line);

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    AstNode *statement = parseStatement(parser);
    if (statement != NULL) csAstProgramAdd(parser->arena, block, statement);
    if (parser->diag->panicMode) synchronize(parser);
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' to close this block");
  return parser->diag->panicMode ? NULL : block;
}

AstNode *parseIf(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'if'");
  AstNode *condition = parseExpression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
  if (condition == NULL || parser->diag->panicMode) return NULL;

  AstNode *thenBranch = parseStatement(parser);
  if (thenBranch == NULL) return NULL;

  AstNode *elseBranch = NULL;
  if (matchToken(parser, TOKEN_ELSE)) {
    elseBranch = parseStatement(parser);
    if (elseBranch == NULL) return NULL;
  }

  return csAstIf(parser->arena, line, condition, thenBranch, elseBranch);
}

/* `do body while (condition);` — the body runs before the first test. */
AstNode *parseDoWhile(Parser *parser) {
  int line = parser->previous.line;

  AstNode *body = parseStatement(parser);
  if (body == NULL) return NULL;

  consume(parser, TOKEN_WHILE, "expected 'while' after the body of a 'do'");
  if (parser->diag->panicMode) return NULL;
  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'while'");
  AstNode *condition = parseExpression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
  if (condition == NULL || parser->diag->panicMode) return NULL;

  /* The trailing semicolon is required in JavaScript and cheap to insist on. */
  consume(parser, TOKEN_SEMICOLON, "expected ';' after 'do ... while (...)'");
  if (parser->diag->panicMode) return NULL;

  AstNode *node = csAstWhile(parser->arena, line, condition, body);
  if (node != NULL) node->as.whileStmt.isDoWhile = true;
  return node;
}

AstNode *parseWhile(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'while'");
  AstNode *condition = parseExpression(parser);
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
  if (condition == NULL || parser->diag->panicMode) return NULL;

  AstNode *body = parseStatement(parser);
  if (body == NULL) return NULL;

  return csAstWhile(parser->arena, line, condition, body);
}

/* `for (init; condition; increment) body` — every clause may be empty. */
AstNode *parseFor(Parser *parser) {
  int line = parser->previous.line;
  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'for'");
  if (parser->diag->panicMode) return NULL;

  AstNode *initializer = NULL;
  if (matchToken(parser, TOKEN_SEMICOLON)) {
    initializer = NULL;
  } else if (check(parser, TOKEN_LET) || check(parser, TOKEN_CONST)) {
    bool isConst = check(parser, TOKEN_CONST);
    advanceToken(parser);

    /* `for (const [k, v] of m)` — a pattern where the name would be. Only a
     * for-of can take one, since there is nothing for a counted loop to
     * destructure. */
    if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
      bool patternIsObject = check(parser, TOKEN_LEFT_BRACE);
      advanceToken(parser);
      AstNode *pattern = parsePattern(parser, patternIsObject, isConst);
      if (pattern == NULL) return NULL;

      bool patternForIn = check(parser, TOKEN_IN);
      bool patternForOf = check(parser, TOKEN_IDENTIFIER) &&
                          parser->current.length == 2 &&
                          memcmp(parser->current.start, "of", 2) == 0;
      if (!patternForOf && !patternForIn) {
        errorAtCurrent(parser, "a pattern here needs 'of' or 'in'");
        return NULL;
      }
      advanceToken(parser);

      AstNode *iterable = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the iterable");
      if (iterable == NULL || parser->diag->panicMode) return NULL;

      AstNode *body = parseStatement(parser);
      if (body == NULL) return NULL;

      /* The binding takes a name no source can write; the pattern unpacks it
       * on each iteration, exactly as a destructured parameter does. */
      AstNode *loop = csAstForOf(parser->arena, line, " element", 8, isConst,
                                 iterable, body);
      if (loop != NULL) {
        loop->as.forOf.isForIn = patternForIn;
        loop->as.forOf.pattern = pattern;
      }
      return loop;
    }

    /* `for (const x of xs)` and `for (let i = 0; ...)` diverge right after the
     * binding name, so the name is read once and the shape decided from what
     * follows it. */
    consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
    if (parser->diag->panicMode) return NULL;
    const char *bindingName = parser->previous.start;
    int bindingLength = parser->previous.length;

    /* `of` is contextual, not a keyword: `Array.of` and a variable called `of`
     * both have to keep working, so it is recognised by its text right here
     * rather than by the lexer. */
    bool isForOf = check(parser, TOKEN_IDENTIFIER) && parser->current.length == 2 &&
                   memcmp(parser->current.start, "of", 2) == 0;
    /* `in` is a real keyword, unlike `of`, because nothing else can follow the
     * binding name here. */
    bool isForIn = check(parser, TOKEN_IN);
    if (isForOf || isForIn) {
      advanceToken(parser);
      AstNode *iterable = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the iterable");
      if (iterable == NULL || parser->diag->panicMode) return NULL;

      AstNode *body = parseStatement(parser);
      if (body == NULL) return NULL;
      AstNode *loop = csAstForOf(parser->arena, line, bindingName, bindingLength,
                                 isConst, iterable, body);
      if (loop != NULL) loop->as.forOf.isForIn = isForIn;
      return loop;
    }

    initializer = finishVarDeclaration(parser, line, bindingName, bindingLength,
                                       isConst);
    if (initializer == NULL) return NULL;
    /* The loop form owns its own semicolon, which the declaration no longer
     * consumes now that `let a = 1, b = 2;` is a list. */
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the loop initialiser");
    if (parser->diag->panicMode) return NULL;
  } else {
    AstNode *expression = parseExpression(parser);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the loop initialiser");
    if (expression == NULL || parser->diag->panicMode) return NULL;
    initializer = csAstExpressionStmt(parser->arena, line, expression);
  }

  AstNode *condition = NULL;
  if (!check(parser, TOKEN_SEMICOLON)) {
    condition = parseExpression(parser);
    if (condition == NULL) return NULL;
  }
  consume(parser, TOKEN_SEMICOLON, "expected ';' after the loop condition");
  if (parser->diag->panicMode) return NULL;

  AstNode *increment = NULL;
  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    increment = parseExpression(parser);
    if (increment == NULL) return NULL;
  }
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the for clauses");
  if (parser->diag->panicMode) return NULL;

  AstNode *body = parseStatement(parser);
  if (body == NULL) return NULL;

  return csAstFor(parser->arena, line, initializer, condition, increment, body);
}

/* Keywords the lexer recognises but no stage implements yet. Naming them beats
 * "expected an expression", which says nothing about why an ordinary line was
 * rejected. */
const char *notImplementedMessage(TokenType type) {
  switch (type) {
    default:
      return NULL;
  }
}

/* The optional label on `break` / `continue`, and the semicolon after it. */
static bool parseJumpLabel(Parser *parser, AstNode *jump, const char *keyword) {
  if (matchToken(parser, TOKEN_IDENTIFIER)) {
    jump->as.jump.label = parser->previous.start;
    jump->as.jump.labelLength = parser->previous.length;
  }

  char message[64];
  snprintf(message, sizeof message, "expected ';' after '%s'", keyword);
  consume(parser, TOKEN_SEMICOLON, message);
  return !parser->diag->panicMode;
}

AstNode *parseStatement(Parser *parser) {
  int line = parser->current.line;

  /* `outer: for (...)` — a label. One token of lookahead separates it from an
   * expression statement that merely starts with a name. */
  if (check(parser, TOKEN_IDENTIFIER)) {
    Lexer probe = parser->lexer;
    if (csLexerNext(&probe).type == TOKEN_COLON) {
      advanceToken(parser);
      const char *name = parser->previous.start;
      int nameLength = parser->previous.length;
      consume(parser, TOKEN_COLON, "expected ':' after a label");
      if (parser->diag->panicMode) return NULL;

      AstNode *body = parseStatement(parser);
      if (body == NULL) return NULL;
      return csAstLabeled(parser->arena, line, name, nameLength, body);
    }
  }

  /* `var` is hoisted and function-scoped in JavaScript, which is the source of
   * a whole family of bugs. CScript has `let` and `const` and nothing else. */
  if (check(parser, TOKEN_VAR)) {
    errorAtCurrent(parser,
                   "'var' is not supported because it is function-scoped and hoisted; "
                   "use 'let' or 'const'");
    return NULL;
  }

  const char *notImplemented = notImplementedMessage(parser->current.type);
  if (notImplemented != NULL) {
    errorAtCurrent(parser, notImplemented);
    return NULL;
  }

  if (matchToken(parser, TOKEN_BREAK)) {
    int breakLine = parser->previous.line;
    AstNode *jump = csAstBreak(parser->arena, breakLine);
    if (!parseJumpLabel(parser, jump, "break")) return NULL;
    return jump;
  }

  if (matchToken(parser, TOKEN_CONTINUE)) {
    int continueLine = parser->previous.line;
    AstNode *jump = csAstContinue(parser->arena, continueLine);
    if (!parseJumpLabel(parser, jump, "continue")) return NULL;
    return jump;
  }

  if (matchToken(parser, TOKEN_THROW)) {
    int throwLine = parser->previous.line;
    AstNode *thrown = parseExpression(parser);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after throw");
    if (thrown == NULL || parser->diag->panicMode) return NULL;
    return csAstThrow(parser->arena, throwLine, thrown);
  }

  if (matchToken(parser, TOKEN_TRY)) return parseTry(parser);

  if (matchToken(parser, TOKEN_SWITCH)) return parseSwitch(parser);

  if (matchToken(parser, TOKEN_FUNCTION)) return parseFunction(parser, true);

  /* `async function f() {}` — contextual, so only an `async` immediately
   * followed by `function` means anything here. */
  if (checkWord(parser, "async") && nextStartsFunction(parser)) {
    advanceToken(parser);
    advanceToken(parser);
    parser->pendingAsync = true;
    return parseFunction(parser, true);
  }

  if (matchToken(parser, TOKEN_CLASS)) return parseClass(parser);

  if (matchToken(parser, TOKEN_DO)) return parseDoWhile(parser);

  if (matchToken(parser, TOKEN_IMPORT)) return parseImport(parser);

  if (matchToken(parser, TOKEN_EXPORT)) return parseExport(parser);

  if (matchToken(parser, TOKEN_RETURN)) {
    int returnLine = parser->previous.line;
    AstNode *value = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
      value = parseExpression(parser);
      if (value == NULL) return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after return");
    if (parser->diag->panicMode) return NULL;
    return csAstReturn(parser->arena, returnLine, value);
  }

  if (matchToken(parser, TOKEN_LET)) return parseVarDeclaration(parser, false);
  if (matchToken(parser, TOKEN_CONST)) return parseVarDeclaration(parser, true);
  if (matchToken(parser, TOKEN_LEFT_BRACE)) return parseBlock(parser);
  if (matchToken(parser, TOKEN_IF)) return parseIf(parser);
  if (matchToken(parser, TOKEN_WHILE)) return parseWhile(parser);
  if (matchToken(parser, TOKEN_FOR)) return parseFor(parser);

  AstNode *expression = parseExpression(parser);
  consume(parser, TOKEN_SEMICOLON, "expected ';' after this statement");
  if (expression == NULL || parser->diag->panicMode) return NULL;
  return csAstExpressionStmt(parser->arena, line, expression);
}
