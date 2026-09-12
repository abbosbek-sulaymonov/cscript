/* parser_primary.c — the operands an expression can start with.
 *
 * Literals, `this`, `super`, `new`, an identifier, a function, an array or
 * object literal, and a parenthesised expression — which is also where an
 * arrow's parameter list hides, and the one place this parser has to look
 * ahead to tell which it is reading.
 *
 * Answers NULL without consuming anything for a token that is not one of its
 * own, so parsePrimary asks each group in turn — which is not the same as
 * failing: a malformed expression it *did* recognise has already reported the
 * error, and answers NULL too. The parser's own position is what tells the two
 * apart, and parsePrimary does not need to.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

AstNode *parseOperandPrimary(Parser *parser, int line) {
  if (matchToken(parser, TOKEN_NUMBER)) {
    return parseCallSuffixes(parser, csAstNumber(parser->arena, line, parseNumberLiteral(parser->previous.start, parser->previous.length)));
  }
  if (matchToken(parser, TOKEN_BIGINT)) {
    /* Minus the `n`, which the lexer included and the digits do not want. */
    return parseCallSuffixes(parser, csAstBigInt(parser->arena, line, parser->previous.start, parser->previous.length - 1));
  }
  if (matchToken(parser, TOKEN_STRING)) {
    return parseCallSuffixes(parser, makeStringLiteral(parser, parser->previous.start, parser->previous.length, line));
  }
  if (matchToken(parser, TOKEN_TEMPLATE)) {
    AstNode *template = parseTemplate(parser, parser->previous.start, parser->previous.length, line);
    if (template == NULL) return NULL;
    return parseCallSuffixes(parser, template);
  }
  /* The literals take suffixes like anything else, which is what makes
   * `null?.x` parse — and it has to, because that is the case `?.` exists
   * for. */
  if (matchToken(parser, TOKEN_TRUE)) {
    return parseCallSuffixes(parser, csAstBool(parser->arena, line, true));
  }
  if (matchToken(parser, TOKEN_FALSE)) {
    return parseCallSuffixes(parser, csAstBool(parser->arena, line, false));
  }
  if (matchToken(parser, TOKEN_NULL)) {
    return parseCallSuffixes(parser, csAstNull(parser->arena, line));
  }
  if (matchToken(parser, TOKEN_UNDEFINED)) {
    return parseCallSuffixes(parser, csAstUndefined(parser->arena, line));
  }

  if (matchToken(parser, TOKEN_THIS)) {
    return parseCallSuffixes(parser, csAstThis(parser->arena, line));
  }

  if (matchToken(parser, TOKEN_SUPER)) {
    if (matchToken(parser, TOKEN_DOT)) {
      consume(parser, TOKEN_IDENTIFIER, "expected a method name after 'super.'");
      if (parser->diag->panicMode) return NULL;
      return parseCallSuffixes(parser, csAstSuper(parser->arena, line, parser->previous.start, parser->previous.length));
    }
    if (check(parser, TOKEN_LEFT_PAREN)) {
      /* `super(...)` — the call suffix below turns it into the constructor
       * call. A NULL name is what distinguishes it from `super.m`. */
      return parseCallSuffixes(parser, csAstSuper(parser->arena, line, NULL, 0));
    }
    errorAtCurrent(parser, "'super' must be followed by '(' or '.'");
    return NULL;
  }

  /* `import(specifier)` — a module named at run time. It is not a call of
   * anything: `import` is a keyword, so this is its own form rather than an
   * identifier that happens to be callable. */
  if (check(parser, TOKEN_IMPORT)) {
    Lexer probe = parser->lexer;
    if (csLexerNext(&probe).type == TOKEN_LEFT_PAREN) {
      advanceToken(parser); /* `import` */
      advanceToken(parser); /* `(` */
      AstNode *specifier = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (specifier == NULL) return NULL;
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the module name");
      if (parser->diag->panicMode) return NULL;
      return parseCallSuffixes(parser, csAstDynamicImport(parser->arena, line, specifier));
    }
  }

  if (matchToken(parser, TOKEN_NEW)) {
    /* Whatever the class is reached through: a name, a property of one, an
     * element of an array, or a parenthesised expression. The one thing it may
     * not swallow is the argument list, which belongs to `new` rather than to
     * the expression naming the class. */
    /* `new.target` is not a construction at all — it is one token pair that
     * asks how the current call was reached. */
    if (check(parser, TOKEN_DOT)) {
      advanceToken(parser);
      if (!consumePropertyName(parser, "expected 'target' after 'new.'")) return NULL;
      if (parser->previous.length != 6 || memcmp(parser->previous.start, "target", 6) != 0) {
        errorAtCurrent(parser, "'new.' is only followed by 'target'");
        return NULL;
      }
      return parseCallSuffixes(parser, csAstNewTarget(parser->arena, line));
    }

    AstNode *callee;
    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      callee = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the expression");
      if (callee == NULL || parser->diag->panicMode) return NULL;
    } else {
      consume(parser, TOKEN_IDENTIFIER, "expected a class after 'new'");
      if (parser->diag->panicMode) return NULL;
      callee = csAstIdentifier(parser->arena, line, parser->previous.start, parser->previous.length);
    }

    /* `new a.B()` and `new registry[0]()`. */
    for (;;) {
      if (matchToken(parser, TOKEN_DOT)) {
        if (!consumePropertyName(parser, "expected a property name after '.'")) {
          return NULL;
        }
        if (parser->diag->panicMode) return NULL;
        callee = csAstProperty(parser->arena, line, callee, parser->previous.start, parser->previous.length);
        continue;
      }
      if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
        AstNode *index = parseExpression(parser);
        consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the index");
        if (index == NULL || parser->diag->panicMode) return NULL;
        callee = csAstIndex(parser->arena, line, callee, index);
        continue;
      }
      break;
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

  /* `async function () { … }` as a value, which is the one place `async` is
   * followed by a keyword rather than by parameters. */
  if (checkWord(parser, "async")) {
    Lexer probe = parser->lexer;
    if (csLexerNext(&probe).type == TOKEN_FUNCTION) {
      advanceToken(parser); /* async */
      advanceToken(parser); /* function */
      parser->pendingAsync = true;
      return parseCallSuffixes(parser, parseFunction(parser, false));
    }
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
      csAstFunctionAddParam(parser->arena, arrow, name, nameLength, TYPE_DYNAMIC, false);
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
          errorAtCurrent(parser,
                         "an array element cannot be left empty; arrays "
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

        /* `{ ...source }`. Order matters — a later entry overwrites an
         * earlier one either way round — so it is kept as an entry with no
         * key rather than hoisted. */
        if (matchToken(parser, TOKEN_ELLIPSIS)) {
          AstNode *source = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (source == NULL) return NULL;
          csAstObjectLiteralAdd(parser->arena, object, NULL, source);
          continue;
        }

        /* `async m() {}` and `async *m() {}`. An `async` followed by anything
         * but `:` or `(` is the modifier rather than the key's own name — the
         * same test a class body makes, for the same reason. */
        bool isAsyncEntry = false;
        if (checkWord(parser, "async")) {
          Lexer probe = parser->lexer;
          Token next = csLexerNext(&probe);
          if (next.type != TOKEN_COLON && next.type != TOKEN_LEFT_PAREN && next.type != TOKEN_COMMA && next.type != TOKEN_RIGHT_BRACE) {
            advanceToken(parser);
            isAsyncEntry = true;
          }
        }

        /* `*m() {}` — a generator method, read before the key. */
        bool isGeneratorEntry = matchToken(parser, TOKEN_STAR);

        /* `get x() {}` — an accessor. `get` is only a modifier when a name
         * follows it; `{ get: 1 }` and `{ get() {} }` are an ordinary property
         * and an ordinary method. */
        ObjectEntryKind entryKind = OBJECT_ENTRY_VALUE;
        if (checkWord(parser, "get") || checkWord(parser, "set")) {
          Lexer probe = parser->lexer;
          Token next = csLexerNext(&probe);
          if (next.type == TOKEN_IDENTIFIER || next.type == TOKEN_STRING) {
            entryKind = parser->current.start[0] == 'g' ? OBJECT_ENTRY_GETTER : OBJECT_ENTRY_SETTER;
            advanceToken(parser);
          }
        }

        /* Keys may be written bare, quoted, or computed. The first two become
         * string constants; a computed one is an expression the VM converts. */
        AstNode *key;
        if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
          key = parseExpression(parser);
          consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after a computed key");
          if (key == NULL || parser->diag->panicMode) return NULL;

          /* `{ [name]() { … } }` — a method whose name is computed, which is
           * how a symbol-keyed method is written. */
          if (check(parser, TOKEN_LEFT_PAREN)) {
            parser->pendingAsync = isAsyncEntry;
            parser->pendingGenerator = isGeneratorEntry;
            AstNode *method = parseFunctionRest(parser, key->line, " computed", 9, true);
            if (method == NULL) return NULL;
            method->as.function.isMethod = true;
            csAstObjectLiteralAddKind(parser->arena, object, key, method, entryKind);
            continue;
          }

          consume(parser, TOKEN_COLON, "expected ':' after the property name");
          if (parser->diag->panicMode) return NULL;
          AstNode *computedValue = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (computedValue == NULL) return NULL;
          csAstObjectLiteralAdd(parser->arena, object, key, computedValue);
          continue;
        }
        if (matchToken(parser, TOKEN_STRING)) {
          key = makeStringLiteral(parser, parser->previous.start, parser->previous.length, parser->previous.line);
        } else if (matchToken(parser, TOKEN_NUMBER)) {
          /* `{ 1: x }`. The key is the number's *string* form, because that is
           * what `o[1]` and `o["1"]` both look up. */
          key = csAstString(parser->arena, parser->previous.line, parser->previous.start, parser->previous.length);
        } else {
          if (!consumePropertyName(parser, "expected a property name")) return NULL;
          if (parser->diag->panicMode) return NULL;
          key = csAstString(parser->arena, parser->previous.line, parser->previous.start, parser->previous.length);
        }

        /* `{ m() {} }` is `{ m: function m() {} }`. Named after the key, so a
         * stack trace says which method it was. */
        if (check(parser, TOKEN_LEFT_PAREN)) {
          parser->pendingAsync = isAsyncEntry;
          parser->pendingGenerator = isGeneratorEntry;
          AstNode *method = parseFunctionRest(parser, key->line, key->as.string.chars, key->as.string.length, true);
          if (method == NULL) return NULL;
          method->as.function.isMethod = true;
          csAstObjectLiteralAddKind(parser->arena, object, key, method, entryKind);
          continue;
        }

        if (entryKind != OBJECT_ENTRY_VALUE) {
          errorAtCurrent(parser, "expected '(' after an accessor name");
          return NULL;
        }

        /* `{ x }` is `{ x: x }`. The key was just read, so the value is an
         * identifier with the same name and the same position. */
        AstNode *value;
        bool shorthand = check(parser, TOKEN_COMMA) || check(parser, TOKEN_RIGHT_BRACE);
        if (shorthand) {
          value = csAstIdentifier(parser->arena, key->line, key->as.string.chars, key->as.string.length);
        } else {
          consume(parser, TOKEN_COLON, "expected ':' after the property name");
          if (parser->diag->panicMode) return NULL;
          value = parsePrecedence(parser, PREC_ASSIGNMENT);
        }
        if (value == NULL) return NULL;

        /* Only the written-out `__proto__: v` links. The shorthand `{ __proto__ }`
         * and the method form `{ __proto__() {} }` are ordinary properties,
         * which is the distinction JavaScript draws too. */
        bool linksPrototype = !shorthand && key->as.string.length == 9 && memcmp(key->as.string.chars, "__proto__", 9) == 0;
        if (linksPrototype) {
          csAstObjectLiteralAddKind(parser->arena, object, key, value, OBJECT_ENTRY_PROTO);
        } else {
          csAstObjectLiteralAdd(parser->arena, object, key, value);
        }
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
            int generatedLength = snprintf(generated, sizeof generated, " arg%d", patternIndex++);
            csAstFunctionAddParam(parser->arena, arrow, generated, generatedLength, TYPE_DYNAMIC, false);
            csAstParamPattern(arrow, pattern);
            continue;
          }
          bool isRest = matchToken(parser, TOKEN_ELLIPSIS);

          consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
          if (parser->diag->panicMode) return NULL;
          const char *paramName = parser->previous.start;
          int paramLength = parser->previous.length;

          TypeKind paramType;
          bool annotated;
          if (!parseTypeAnnotation(parser, &paramType, &annotated)) return NULL;
          csAstFunctionAddParam(parser->arena, arrow, paramName, paramLength, paramType, annotated);
          if (isRest) {
            arrow->as.function.hasRest = true;
            if (check(parser, TOKEN_COMMA)) {
              errorAtCurrent(parser, "a rest parameter has to be the last one");
              return NULL;
            }
          }

          if (matchToken(parser, TOKEN_EQUAL)) {
            AstNode *fallback = parsePrecedence(parser, PREC_ASSIGNMENT);
            if (fallback == NULL) return NULL;
            arrow->as.function.params[arrow->as.function.paramCount - 1].defaultValue = fallback;
          }
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
  return NULL;
}
