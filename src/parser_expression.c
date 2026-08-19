/* parser_expression.c — expressions, and the functions that are expressions.
 *
 * Precedence climbing over the operator table, plus everything that can\n * appear where a value is wanted: literals, calls, property access, object and\n * array literals, template strings, arrow functions and function expressions.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "parser_internal.h"


/* Postfix `.name` and `(args)`, which bind tighter than any unary operator. */
AstNode *parseCallSuffixes(Parser *parser, AstNode *expression) {
  for (;;) {
    if (matchToken(parser, TOKEN_DOT)) {
      int line = parser->previous.line;
      if (!consumePropertyName(parser, "expected a property name after '.'")) return NULL;
      if (parser->diag->panicMode) return NULL;
      expression = csAstProperty(parser->arena, line, expression,
                                 parser->previous.start, parser->previous.length);
      continue;
    }

    if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
      int line = parser->previous.line;
      AstNode *index = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the index");
      if (index == NULL || parser->diag->panicMode) return NULL;
      expression = csAstIndex(parser->arena, line, expression, index);
      continue;
    }

    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      int line = parser->previous.line;
      AstNode *call = csAstCall(parser->arena, line, expression);
      if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
          bool isSpread = matchToken(parser, TOKEN_ELLIPSIS);
          AstNode *argument = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (argument == NULL) return NULL;
          if (isSpread) argument = csAstSpread(parser->arena, line, argument);
          csAstCallAddArgument(parser->arena, call, argument);
        } while (matchToken(parser, TOKEN_COMMA));
      }
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after arguments");
      if (parser->diag->panicMode) return NULL;
      expression = call;
      continue;
    }

    /* Postfix ++/--. It binds tighter than any unary operator, and yields the
     * value from *before* the update, which is why it needs its own node
     * rather than desugaring to an assignment. */
    if (check(parser, TOKEN_PLUS_PLUS) || check(parser, TOKEN_MINUS_MINUS)) {
      bool isIncrement = check(parser, TOKEN_PLUS_PLUS);
      int line = parser->current.line;
      if (expression->type != AST_IDENTIFIER) {
        errorAtCurrent(parser, "'++' and '--' need a variable to update");
        return NULL;
      }
      advanceToken(parser);
      expression = csAstUpdate(parser->arena, line, expression, isIncrement, false);
      continue;
    }

    return expression;
  }
}

/* Literals, identifiers, prefix operators and parenthesised groups. */
AstNode *parsePrimary(Parser *parser) {
  int line = parser->current.line;

  if (matchToken(parser, TOKEN_NUMBER)) {
    return parseCallSuffixes(
        parser, csAstNumber(parser->arena, line,
                            parseNumberLiteral(parser->previous.start,
                                               parser->previous.length)));
  }
  if (matchToken(parser, TOKEN_STRING)) {
    return parseCallSuffixes(
        parser, makeStringLiteral(parser, parser->previous.start,
                                  parser->previous.length, line));
  }
  if (matchToken(parser, TOKEN_TEMPLATE)) {
    AstNode *template = parseTemplate(parser, parser->previous.start,
                                      parser->previous.length, line);
    if (template == NULL) return NULL;
    return parseCallSuffixes(parser, template);
  }
  if (matchToken(parser, TOKEN_TRUE)) {
    return csAstBool(parser->arena, line, true);
  }
  if (matchToken(parser, TOKEN_FALSE)) {
    return csAstBool(parser->arena, line, false);
  }
  if (matchToken(parser, TOKEN_NULL)) return csAstNull(parser->arena, line);
  if (matchToken(parser, TOKEN_UNDEFINED)) return csAstUndefined(parser->arena, line);

  if (matchToken(parser, TOKEN_THIS)) {
    return parseCallSuffixes(parser, csAstThis(parser->arena, line));
  }

  if (matchToken(parser, TOKEN_SUPER)) {
    if (matchToken(parser, TOKEN_DOT)) {
      consume(parser, TOKEN_IDENTIFIER, "expected a method name after 'super.'");
      if (parser->diag->panicMode) return NULL;
      return parseCallSuffixes(
          parser, csAstSuper(parser->arena, line, parser->previous.start,
                             parser->previous.length));
    }
    if (check(parser, TOKEN_LEFT_PAREN)) {
      /* `super(...)` — the call suffix below turns it into the constructor
       * call. A NULL name is what distinguishes it from `super.m`. */
      return parseCallSuffixes(parser, csAstSuper(parser->arena, line, NULL, 0));
    }
    errorAtCurrent(parser, "'super' must be followed by '(' or '.'");
    return NULL;
  }

  if (matchToken(parser, TOKEN_NEW)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a class name after 'new'");
    if (parser->diag->panicMode) return NULL;

    AstNode *callee = csAstIdentifier(parser->arena, line, parser->previous.start,
                                      parser->previous.length);
    /* `new a.B()` — a class reached through a property. */
    while (matchToken(parser, TOKEN_DOT)) {
      if (!consumePropertyName(parser, "expected a property name after '.'")) return NULL;
      if (parser->diag->panicMode) return NULL;
      callee = csAstProperty(parser->arena, line, callee, parser->previous.start,
                             parser->previous.length);
    }

    AstNode *node = csAstNew(parser->arena, line, callee);
    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
          if (check(parser, TOKEN_ELLIPSIS)) {
            errorAtCurrent(parser, "spreading arguments into 'new' is not supported yet");
            return NULL;
          }
          AstNode *argument = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (argument == NULL) return NULL;
          csAstCallAddArgument(parser->arena, node, argument);
        } while (matchToken(parser, TOKEN_COMMA));
      }
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the constructor arguments");
      if (parser->diag->panicMode) return NULL;
    }
    return parseCallSuffixes(parser, node);
  }

  /* `async x => ...` and `async (a, b) => ...`. Only an `async` that is
   * actually followed by parameters means anything; anywhere else it is a
   * variable named `async`. */
  if (checkWord(parser, "async") && nextStartsArrowParams(parser)) {
    advanceToken(parser);
    parser->pendingAsync = true;
    AstNode *arrow = parsePrimary(parser);
    parser->pendingAsync = false;
    return arrow;
  }

  if (matchToken(parser, TOKEN_IDENTIFIER)) {
    const char *name = parser->previous.start;
    int nameLength = parser->previous.length;

    /* `x => ...` — a single parameter needs no parentheses. */
    if (check(parser, TOKEN_ARROW)) {
      advanceToken(parser);
      AstNode *arrow = csAstFunction(parser->arena, line, NULL, 0);
      arrow->as.function.isAsync = parser->pendingAsync;
      csAstFunctionAddParam(parser->arena, arrow, name, nameLength, TYPE_ANY, false);
      return finishArrow(parser, arrow, line);
    }

    AstNode *identifier = csAstIdentifier(parser->arena, line, name, nameLength);
    return parseCallSuffixes(parser, identifier);
  }

  if (matchToken(parser, TOKEN_FUNCTION)) {
    AstNode *function = parseFunction(parser, false);
    if (function == NULL) return NULL;
    return parseCallSuffixes(parser, function);
  }

  if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
    AstNode *array = csAstArrayLiteral(parser->arena, line);
    if (!check(parser, TOKEN_RIGHT_BRACKET)) {
      do {
        /* A trailing comma before ']' is allowed, as in JavaScript. */
        if (check(parser, TOKEN_RIGHT_BRACKET)) break;

        /* `[1, , 2]` is a hole. Arrays here are dense on purpose — holes are
         * the reason engines need a second, slower array representation. */
        if (check(parser, TOKEN_COMMA)) {
          errorAtCurrent(parser, "an array element cannot be left empty; arrays "
                                 "are dense and have no holes");
          return NULL;
        }

        bool isSpread = matchToken(parser, TOKEN_ELLIPSIS);
        AstNode *element = parsePrecedence(parser, PREC_ASSIGNMENT);
        if (element == NULL) return NULL;
        if (isSpread) element = csAstSpread(parser->arena, line, element);
        csAstArrayLiteralAdd(parser->arena, array, element);
      } while (matchToken(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the array elements");
    if (parser->diag->panicMode) return NULL;
    return parseCallSuffixes(parser, array);
  }

  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    AstNode *object = csAstObjectLiteral(parser->arena, line);
    if (!check(parser, TOKEN_RIGHT_BRACE)) {
      do {
        if (check(parser, TOKEN_RIGHT_BRACE)) break;

        /* `get x() {}` in an object literal is an accessor, which only classes
         * support — worth naming rather than failing at the missing colon. */
        if (checkWord(parser, "get") || checkWord(parser, "set")) {
          Lexer probe = parser->lexer;
          Token next = csLexerNext(&probe);
          if (next.type == TOKEN_IDENTIFIER) {
            errorAtCurrent(parser, "getters and setters on an object literal are not "
                                   "supported yet; declare a class instead");
            return NULL;
          }
        }

        /* Keys may be written bare, quoted, or computed. The first two become
         * string constants; a computed one is an expression the VM converts. */
        AstNode *key;
        if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
          key = parseExpression(parser);
          consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after a computed key");
          if (key == NULL || parser->diag->panicMode) return NULL;
          consume(parser, TOKEN_COLON, "expected ':' after the property name");
          if (parser->diag->panicMode) return NULL;
          AstNode *computedValue = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (computedValue == NULL) return NULL;
          csAstObjectLiteralAdd(parser->arena, object, key, computedValue);
          continue;
        }
        if (matchToken(parser, TOKEN_STRING)) {
          key = makeStringLiteral(parser, parser->previous.start,
                                  parser->previous.length, parser->previous.line);
        } else {
          if (!consumePropertyName(parser, "expected a property name")) return NULL;
          if (parser->diag->panicMode) return NULL;
          key = csAstString(parser->arena, parser->previous.line,
                            parser->previous.start, parser->previous.length);
        }

        /* `{ x }` is `{ x: x }`. The key was just read, so the value is an
         * identifier with the same name and the same position. */
        AstNode *value;
        if (check(parser, TOKEN_COMMA) || check(parser, TOKEN_RIGHT_BRACE)) {
          value = csAstIdentifier(parser->arena, key->line, key->as.string.chars,
                                  key->as.string.length);
        } else {
          consume(parser, TOKEN_COLON, "expected ':' after the property name");
          if (parser->diag->panicMode) return NULL;
          value = parsePrecedence(parser, PREC_ASSIGNMENT);
        }
        if (value == NULL) return NULL;
        csAstObjectLiteralAdd(parser->arena, object, key, value);
      } while (matchToken(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the object literal");
    if (parser->diag->panicMode) return NULL;
    return parseCallSuffixes(parser, object);
  }

  if (matchToken(parser, TOKEN_LEFT_PAREN)) {
    if (looksLikeArrowParams(parser)) {
      AstNode *arrow = csAstFunction(parser->arena, line, NULL, 0);
      arrow->as.function.isAsync = parser->pendingAsync;

      if (!check(parser, TOKEN_RIGHT_PAREN)) {
        int patternIndex = 0;
        do {
          if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
            bool patternIsObject = check(parser, TOKEN_LEFT_BRACE);
            advanceToken(parser);
            AstNode *pattern = parsePattern(parser, patternIsObject, true);
            if (pattern == NULL) return NULL;

            char generated[16];
            int generatedLength =
                snprintf(generated, sizeof generated, " arg%d", patternIndex++);
            csAstFunctionAddParam(parser->arena, arrow, generated, generatedLength,
                                  TYPE_ANY, false);
            csAstParamPattern(arrow, pattern);
            continue;
          }
          consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
          if (parser->diag->panicMode) return NULL;
          const char *paramName = parser->previous.start;
          int paramLength = parser->previous.length;

          TypeKind paramType;
          bool annotated;
          if (!parseTypeAnnotation(parser, &paramType, &annotated)) return NULL;
          csAstFunctionAddParam(parser->arena, arrow, paramName, paramLength,
                                paramType, annotated);
        } while (matchToken(parser, TOKEN_COMMA));
      }
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the parameters");

      TypeKind returnType;
      bool hasReturnAnnotation;
      if (!parseTypeAnnotation(parser, &returnType, &hasReturnAnnotation)) return NULL;
      arrow->as.function.returnType = returnType;
      arrow->as.function.hasReturnAnnotation = hasReturnAnnotation;

      consume(parser, TOKEN_ARROW, "expected '=>' after the parameters");
      if (parser->diag->panicMode) return NULL;
      return finishArrow(parser, arrow, line);
    }

    AstNode *inner = parseExpression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after expression");
    if (inner == NULL) return NULL;
    return parseCallSuffixes(parser, csAstGrouping(parser->arena, line, inner));
  }

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

  if (check(parser, TOKEN_DELETE)) {
    errorAtCurrent(parser, "'delete' is not supported; an object's shape is fixed "
                           "once it is built");
    return NULL;
  }

  if (check(parser, TOKEN_SLASH)) {
    errorAtCurrent(parser, "regular expressions are not supported yet");
    return NULL;
  }

  if (rejectLooseEquality(parser)) return NULL;

  errorAtCurrent(parser, "expected an expression");
  return NULL;
}

/* Precedence climbing: parse a primary, then keep folding in operators whose
 * precedence is at least `minPrecedence`. Binary operators here are
 * left-associative, so their right side is parsed one level tighter;
 * assignment is right-associative and so parses at its own level. */
AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence) {
  AstNode *left = parsePrimary(parser);
  if (left == NULL) return NULL;

  for (;;) {
    /* Assignment, handled before the table because it is right-associative and
     * needs the left side to be a valid target rather than a value. */
    BinaryOp compound;
    bool isPlain = check(parser, TOKEN_EQUAL);
    bool isCompound = compoundAssignOp(parser->current.type, &compound);

    if ((isPlain || isCompound) && minPrecedence <= PREC_ASSIGNMENT) {
      int line = parser->current.line;
      advanceToken(parser);

      if (left->type != AST_IDENTIFIER && left->type != AST_PROPERTY &&
          left->type != AST_INDEX) {
        csDiagnosticError(parser->diag, line, NULL, 0,
                          "the left side of an assignment must be a variable, "
                          "a property or an index");
        return NULL;
      }

      AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (value == NULL) return NULL;
      if (isCompound) value = csAstBinary(parser->arena, line, compound, left, value);

      left = csAstAssign(parser->arena, line, left, value);
      continue;
    }

    /* `?:` is right-associative and binds looser than everything except
     * assignment, so both arms parse at the conditional level. */
    if (check(parser, TOKEN_QUESTION) && minPrecedence <= PREC_CONDITIONAL) {
      int line = parser->current.line;
      advanceToken(parser);

      AstNode *thenValue = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (thenValue == NULL) return NULL;
      consume(parser, TOKEN_COLON, "expected ':' in a conditional expression");
      if (parser->diag->panicMode) return NULL;
      AstNode *elseValue = parsePrecedence(parser, PREC_CONDITIONAL);
      if (elseValue == NULL) return NULL;

      left = csAstConditional(parser->arena, line, left, thenValue, elseValue);
      continue;
    }

    if (rejectLooseEquality(parser)) return NULL;
    if (rejectInOperator(parser)) return NULL;

    Precedence precedence = binaryPrecedence(parser->current.type);
    if (precedence == PREC_NONE || precedence < minPrecedence) break;

    TokenType operatorType = parser->current.type;
    int line = parser->current.line;
    advanceToken(parser);

    /* ** is the one right-associative binary operator, so 2 ** 3 ** 2 groups
     * as 2 ** (3 ** 2). Every other operator parses its right side one level
     * tighter, which is what makes them left-associative. */
    Precedence rightPrecedence =
        operatorType == TOKEN_STAR_STAR ? precedence : (Precedence)(precedence + 1);
    AstNode *right = parsePrecedence(parser, rightPrecedence);
    if (right == NULL) return NULL;

    if (operatorType == TOKEN_AMP_AMP) {
      left = csAstLogical(parser->arena, line, LOGICAL_AND, left, right);
    } else if (operatorType == TOKEN_PIPE_PIPE) {
      left = csAstLogical(parser->arena, line, LOGICAL_OR, left, right);
    } else {
      left = csAstBinary(parser->arena, line, binaryOpFor(operatorType), left, right);
    }
    if (left == NULL) return NULL;
  }

  return left;
}

AstNode *parseExpression(Parser *parser) {
  return parsePrecedence(parser, PREC_ASSIGNMENT);
}

/* Builds the concatenation a template literal desugars to.
 *
 * `\`a ${x} b\`` becomes `"a" + x + " b"`, which reuses the existing string
 * semantics of `+` — including the rule that a string on either side wins — so
 * interpolating a number needs no extra machinery.
 *
 * The whole literal arrived as one token, so each `${...}` is re-lexed here by
 * a nested parser over a NUL-terminated copy. Its diagnostics share this
 * parser's, and line numbers are reported relative to the template's own line,
 * which is exact unless the template spans lines. */
AstNode *parseTemplate(Parser *parser, const char *start, int length, int line) {
  const char *cursor = start + 1;      /* skip the opening backtick */
  const char *end = start + length - 1; /* and the closing one */

  AstNode *result = NULL;
  char chunk[1024];

  while (cursor < end) {
    /* Literal run up to the next interpolation. */
    int chunkLength = 0;
    while (cursor < end && !(cursor[0] == '$' && cursor + 1 < end && cursor[1] == '{')) {
      char c = *cursor++;
      if (c == '\\' && cursor < end) {
        char escaped = *cursor++;
        switch (escaped) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case '`': c = '`'; break;
          case '$': c = '$'; break;
          case '\\': c = '\\'; break;
          default: c = escaped; break;
        }
      }
      if (chunkLength < (int)sizeof(chunk) - 1) chunk[chunkLength++] = c;
    }

    /* An empty leading chunk still matters: it forces the result to be a
     * string even when the template starts with an interpolation. */
    if (chunkLength > 0 || result == NULL) {
      AstNode *piece = csAstString(parser->arena, line, chunk, chunkLength);
      result = result == NULL
                   ? piece
                   : csAstBinary(parser->arena, line, BINARY_ADD, result, piece);
    }

    if (cursor >= end) break;

    cursor += 2; /* consume "${" */
    const char *exprStart = cursor;
    int depth = 1;
    while (cursor < end && depth > 0) {
      if (*cursor == '{') depth++;
      if (*cursor == '}') depth--;
      if (depth > 0) cursor++;
    }
    if (depth != 0) {
      csDiagnosticError(parser->diag, line, start, length,
                        "unterminated '${' in template literal");
      return NULL;
    }

    int exprLength = (int)(cursor - exprStart);
    cursor++; /* consume "}" */

    char *source = (char *)csAstArenaAlloc(parser->arena, (size_t)exprLength + 1);
    if (source == NULL) return NULL;
    memcpy(source, exprStart, (size_t)exprLength);
    source[exprLength] = '\0';

    Parser nested;
    nested.arena = parser->arena;
    nested.diag = parser->diag;
    csLexerInit(&nested.lexer, source, parser->diag);
    nested.lexer.line = line;
    nested.previous = parser->previous;
    advanceToken(&nested);

    AstNode *expression = parseExpression(&nested);
    if (expression == NULL) return NULL;
    if (!check(&nested, TOKEN_EOF)) {
      csDiagnosticError(parser->diag, line, start, length,
                        "unexpected trailing text in a template interpolation");
      return NULL;
    }

    result = csAstBinary(parser->arena, line, BINARY_ADD, result, expression);
  }

  return result != NULL ? result : csAstString(parser->arena, line, "", 0);
}

/* Looks past a '(' for the ')' that closes it and reports whether '=>' follows.
 *
 * `(a, b)` is a parameter list or a parenthesised expression, and nothing
 * before the arrow distinguishes them. Rather than backtracking the parser,
 * this scans the raw token stream with a throwaway lexer — cheap, because the
 * span is short, and it leaves the real parser's state untouched. */
bool looksLikeArrowParams(Parser *parser) {
  Lexer probe = parser->lexer;
  Diagnostics quiet;
  csDiagnosticsInit(&quiet, NULL, parser->diag->sourceName);
  probe.diag = &quiet;

  /* parser->current is the token after '(', and probe is positioned after it. */
  int depth = 1;
  Token token = parser->current;
  for (;;) {
    if (token.type == TOKEN_EOF) return false;
    if (token.type == TOKEN_LEFT_PAREN) depth++;
    if (token.type == TOKEN_RIGHT_PAREN) {
      depth--;
      if (depth == 0) break;
    }
    token = csLexerNext(&probe);
  }

  Token next = csLexerNext(&probe);

  /* A return-type annotation sits between the ')' and the arrow, so skip it
   * before deciding: `(a: number): number => ...` is still a parameter list. */
  if (next.type == TOKEN_COLON) {
    csLexerNext(&probe); /* the type name */
    next = csLexerNext(&probe);
  }

  return next.type == TOKEN_ARROW;
}

/* Parses an arrow function's body: either an expression, which becomes an
 * implicit return, or a braced block. */
AstNode *finishArrow(Parser *parser, AstNode *function, int line) {
  /* An arrow's body may await only if the arrow itself is async — and a plain
   * arrow nested inside an async function may not, which is why this resets to
   * zero rather than leaving the enclosing depth alone. */
  int enclosingAsync = parser->asyncDepth;
  parser->asyncDepth = function->as.function.isAsync ? enclosingAsync + 1 : 0;

  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    function->as.function.body = parseBlock(parser);
  } else {
    AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
    if (value == NULL) {
      parser->asyncDepth = enclosingAsync;
      return NULL;
    }
    /* `x => expr` is `x => { return expr; }`. */
    AstNode *body = csAstBlock(parser->arena, line);
    csAstProgramAdd(parser->arena, body, csAstReturn(parser->arena, line, value));
    function->as.function.body = body;
  }

  parser->asyncDepth = enclosingAsync;
  return function->as.function.body != NULL ? function : NULL;
}

/* Everything after the name: the parameter list, the return annotation and the
 * body. Shared by function declarations and class methods, which differ only
 * in how their name is introduced. */
AstNode *parseFunctionRest(Parser *parser, int line, const char *name,
                                  int nameLength, bool isMethod) {
  AstNode *function = csAstFunction(parser->arena, line, name, nameLength);
  function->as.function.isAsync = parser->pendingAsync;
  parser->pendingAsync = false;

  consume(parser, TOKEN_LEFT_PAREN,
          isMethod ? "expected '(' after the method name"
                   : "expected '(' after the function name");
  if (parser->diag->panicMode) return NULL;

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    int patternIndex = 0;
    do {
      /* `function f({ a, b })` — the parameter takes a name no source can
       * write, and the pattern is destructured from it at the top of the body.
       * That is exactly what writing it out by hand would produce. */
      if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
        bool patternIsObject = check(parser, TOKEN_LEFT_BRACE);
        advanceToken(parser);
        AstNode *pattern = parsePattern(parser, patternIsObject, true);
        if (pattern == NULL) return NULL;

        char generated[16];
        int generatedLength = snprintf(generated, sizeof generated, " arg%d", patternIndex++);
        csAstFunctionAddParam(parser->arena, function, generated, generatedLength,
                              TYPE_ANY, false);
        csAstParamPattern(function, pattern);
        continue;
      }

      consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
      if (parser->diag->panicMode) return NULL;

      const char *paramName = parser->previous.start;
      int paramLength = parser->previous.length;

      TypeKind paramType;
      bool annotated;
      if (!parseTypeAnnotation(parser, &paramType, &annotated)) return NULL;

      csAstFunctionAddParam(parser->arena, function, paramName, paramLength, paramType,
                            annotated);
    } while (matchToken(parser, TOKEN_COMMA));
  }
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the parameters");
  if (parser->diag->panicMode) return NULL;

  TypeKind returnType;
  bool hasReturnAnnotation;
  if (!parseTypeAnnotation(parser, &returnType, &hasReturnAnnotation)) return NULL;
  function->as.function.returnType = returnType;
  function->as.function.hasReturnAnnotation = hasReturnAnnotation;

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' to open the function body");
  if (parser->diag->panicMode) return NULL;

  int enclosingAsync = parser->asyncDepth;
  parser->asyncDepth = function->as.function.isAsync ? enclosingAsync + 1 : 0;
  function->as.function.body = parseBlock(parser);
  parser->asyncDepth = enclosingAsync;
  if (function->as.function.body == NULL) return NULL;

  return function;
}

/* `function name(a: number, b): number { ... }`
 *
 * A named declaration binds the closure; an anonymous one is an expression. */
AstNode *parseFunction(Parser *parser, bool requireName) {
  int line = parser->previous.line;

  const char *name = NULL;
  int nameLength = 0;
  if (check(parser, TOKEN_IDENTIFIER)) {
    advanceToken(parser);
    name = parser->previous.start;
    nameLength = parser->previous.length;
  } else if (requireName) {
    errorAtCurrent(parser, "expected a function name");
    return NULL;
  }

  return parseFunctionRest(parser, line, name, nameLength, false);
}
