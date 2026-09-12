/* parser_arrow.c — template literals, arrow functions and function forms.
 *
 * The arrow is the one construct this parser cannot decide from its first
 * token: `(a, b)` is a parenthesised expression until a `=>` follows it, and
 * `(a, b) => c` is a parameter list from the start. looksLikeArrowParams is
 * the lookahead that settles it, and finishArrow reinterprets what was already
 * parsed rather than parsing it twice.
 *
 * A template is here because it is the same kind of problem in miniature: one
 * scan produces both the cooked and the raw pieces, because a tagged template
 * wants both and scanning twice would read the escapes twice.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

/* Walks a template literal once, collecting the literal pieces and the
 * expressions between them.
 *
 * Both users need the same walk: an untagged template joins the pieces with
 * `+`, and a tagged one hands them to a function as an array. `raw` keeps the
 * text exactly as written, escapes and all, which is the only thing `String.raw`
 * is for.
 *
 * The whole literal arrived as one token, so each `${...}` is re-lexed here by
 * a nested parser over a NUL-terminated copy. Its diagnostics share this
 * parser's, and line numbers are reported relative to the template's own line,
 * which is exact unless the template spans lines. */
static bool scanTemplate(Parser *parser, const char *start, int length, int line,
                         AstNode ***cookedOut, AstNode ***rawOut,
                         AstNode ***expressionsOut, int *pieceCount) {
  const char *cursor = start + 1;      /* skip the opening backtick */
  const char *end = start + length - 1; /* and the closing one */

  AstNode **cooked = NULL;
  AstNode **raw = NULL;
  AstNode **expressions = NULL;
  int pieces = 0;

  for (;;) {
    char chunk[1024];
    int chunkLength = 0;
    const char *rawStart = cursor;

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

    AstNode **grownCooked =
        (AstNode **)csAstArenaAlloc(parser->arena, sizeof(AstNode *) * (size_t)(pieces + 1));
    AstNode **grownRaw =
        (AstNode **)csAstArenaAlloc(parser->arena, sizeof(AstNode *) * (size_t)(pieces + 1));
    if (grownCooked == NULL || grownRaw == NULL) return false;
    if (pieces > 0) {
      memcpy(grownCooked, cooked, sizeof(AstNode *) * (size_t)pieces);
      memcpy(grownRaw, raw, sizeof(AstNode *) * (size_t)pieces);
    }
    grownCooked[pieces] = csAstString(parser->arena, line, chunk, chunkLength);
    grownRaw[pieces] =
        csAstString(parser->arena, line, rawStart, (int)(cursor - rawStart));
    cooked = grownCooked;
    raw = grownRaw;
    pieces++;

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
      return false;
    }

    int exprLength = (int)(cursor - exprStart);
    cursor++; /* consume "}" */

    char *source = (char *)csAstArenaAlloc(parser->arena, (size_t)exprLength + 1);
    if (source == NULL) return false;
    memcpy(source, exprStart, (size_t)exprLength);
    source[exprLength] = '\0';

    /* The whole parser is copied, not three of its fields: an interpolation
     * inside an async function may await, and inside a generator may yield. */
    Parser nested = *parser;
    csLexerInit(&nested.lexer, source, parser->diag);
    nested.lexer.line = line;
    advanceToken(&nested);

    AstNode *expression = parseExpression(&nested);
    if (expression == NULL) return false;
    if (!check(&nested, TOKEN_EOF)) {
      csDiagnosticError(parser->diag, line, start, length,
                        "unexpected trailing text in a template interpolation");
      return false;
    }

    AstNode **grown = (AstNode **)csAstArenaAlloc(
        parser->arena, sizeof(AstNode *) * (size_t)pieces);
    if (grown == NULL) return false;
    if (pieces > 1) memcpy(grown, expressions, sizeof(AstNode *) * (size_t)(pieces - 1));
    grown[pieces - 1] = expression;
    expressions = grown;
  }

  *cookedOut = cooked;
  *rawOut = raw;
  *expressionsOut = expressions;
  *pieceCount = pieces;
  return true;
}

/* Builds the concatenation an untagged template desugars to.
 *
 * `` `a ${x} b` `` becomes `"a" + x + " b"`, which reuses the existing string
 * semantics of `+` — including the rule that a string on either side wins — so
 * interpolating a number needs no extra machinery. */
AstNode *parseTemplate(Parser *parser, const char *start, int length, int line) {
  AstNode **cooked;
  AstNode **raw;
  AstNode **expressions;
  int pieces;
  if (!scanTemplate(parser, start, length, line, &cooked, &raw, &expressions,
                    &pieces)) {
    return NULL;
  }

  /* The first piece is always emitted, even when empty: it is what forces the
   * result to be a string when the template starts with an interpolation. */
  AstNode *result = cooked[0];
  for (int i = 1; i < pieces; i++) {
    result = csAstBinary(parser->arena, line, BINARY_ADD, result, expressions[i - 1]);
    if (cooked[i]->as.string.length > 0) {
      result = csAstBinary(parser->arena, line, BINARY_ADD, result, cooked[i]);
    }
  }
  return result;
}

/* `` tag`a${x}b` `` — a call, with the pieces as its first argument. */
AstNode *parseTaggedTemplate(Parser *parser, AstNode *tag, const char *start,
                             int length, int line) {
  AstNode **cooked;
  AstNode **raw;
  AstNode **expressions;
  int pieces;
  if (!scanTemplate(parser, start, length, line, &cooked, &raw, &expressions,
                    &pieces)) {
    return NULL;
  }

  AstNode *cookedArray = csAstArrayLiteral(parser->arena, line);
  AstNode *rawArray = csAstArrayLiteral(parser->arena, line);
  for (int i = 0; i < pieces; i++) {
    csAstArrayLiteralAdd(parser->arena, cookedArray, cooked[i]);
    csAstArrayLiteralAdd(parser->arena, rawArray, raw[i]);
  }

  AstNode *strings = csAstTemplateStrings(parser->arena, line, cookedArray, rawArray);
  AstNode *call = csAstCall(parser->arena, line, tag);
  csAstCallAddArgument(parser->arena, call, strings);
  for (int i = 0; i + 1 < pieces; i++) {
    csAstCallAddArgument(parser->arena, call, expressions[i]);
  }
  return call;
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
  function->as.function.isArrow = true;
  /* An arrow's body may await only if the arrow itself is async — and a plain
   * arrow nested inside an async function may not, which is why this resets to
   * zero rather than leaving the enclosing depth alone. */
  int enclosingAsync = parser->asyncDepth;
  parser->asyncDepth = function->as.function.isAsync ? enclosingAsync + 1 : 0;
  /* There is no such thing as a generator arrow, so a `yield` written in one
   * belongs to nothing. */
  bool enclosingGenerator = parser->inGenerator;
  parser->inGenerator = false;

  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    function->as.function.body = parseBlock(parser);
  } else {
    AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
    if (value == NULL) {
      parser->asyncDepth = enclosingAsync;
      parser->inGenerator = enclosingGenerator;
      return NULL;
    }
    /* `x => expr` is `x => { return expr; }`. */
    AstNode *body = csAstBlock(parser->arena, line);
    csAstProgramAdd(parser->arena, body, csAstReturn(parser->arena, line, value));
    function->as.function.body = body;
  }

  parser->asyncDepth = enclosingAsync;
  parser->inGenerator = enclosingGenerator;
  return function->as.function.body != NULL ? function : NULL;
}

/* Everything after the name: the parameter list, the return annotation and the
 * body. Shared by function declarations and class methods, which differ only
 * in how their name is introduced. */
AstNode *parseFunctionRest(Parser *parser, int line, const char *name,
                                  int nameLength, bool isMethod) {
  AstNode *function = csAstFunction(parser->arena, line, name, nameLength);
  function->as.function.isAsync = parser->pendingAsync;
  function->as.function.isGenerator = parser->pendingGenerator;
  bool wasAsync = parser->pendingAsync;
  bool wasGenerator = parser->pendingGenerator;
  parser->pendingAsync = false;
  parser->pendingGenerator = false;

  (void)wasAsync;
  (void)wasGenerator;

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
                              TYPE_DYNAMIC, false);
        csAstParamPattern(function, pattern);
        continue;
      }

      /* `...rest` collects the arguments past every parameter before it. */
      bool isRest = matchToken(parser, TOKEN_ELLIPSIS);

      consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
      if (parser->diag->panicMode) return NULL;

      const char *paramName = parser->previous.start;
      int paramLength = parser->previous.length;

      TypeKind paramType;
      bool annotated;
      if (!parseTypeAnnotation(parser, &paramType, &annotated)) return NULL;

      csAstFunctionAddParam(parser->arena, function, paramName, paramLength, paramType,
                            annotated);
      if (isRest) {
        function->as.function.hasRest = true;
        if (check(parser, TOKEN_COMMA)) {
          errorAtCurrent(parser, "a rest parameter has to be the last one");
          return NULL;
        }
      }

      /* `function f(a = 1)`. The expression is kept on the parameter and run
       * at the top of the body, so it can refer to the parameters before it —
       * which is what `function f(a, b = a * 2)` means. */
      if (matchToken(parser, TOKEN_EQUAL)) {
        AstNode *fallback = parsePrecedence(parser, PREC_ASSIGNMENT);
        if (fallback == NULL) return NULL;
        function->as.function.params[function->as.function.paramCount - 1]
            .defaultValue = fallback;
      }
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
  bool enclosingGenerator = parser->inGenerator;
  parser->inGenerator = function->as.function.isGenerator;

  function->as.function.body = parseBlock(parser);

  parser->asyncDepth = enclosingAsync;
  parser->inGenerator = enclosingGenerator;
  if (function->as.function.body == NULL) return NULL;

  return function;
}

/* `function name(a: number, b): number { ... }`
 *
 * A named declaration binds the closure; an anonymous one is an expression. */
AstNode *parseFunction(Parser *parser, bool requireName) {
  int line = parser->previous.line;

  bool isGenerator = matchToken(parser, TOKEN_STAR);

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

  parser->pendingGenerator = isGenerator;
  AstNode *function = parseFunctionRest(parser, line, name, nameLength, false);
  /* `requireName` is exactly statement position, which is exactly where a
   * function declares its name. */
  if (function != NULL) function->as.function.isDeclaration = requireName;
  return function;
}
