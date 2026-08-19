#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/parser.h"

/* Binding powers, lowest binds loosest. Mirrors JS operator precedence for the
 * operators that exist today; new levels slot in without touching the parser
 * functions, because binary parsing is driven by this table. */
typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  /* = += -= *= /= %=  (right-associative) */
  PREC_CONDITIONAL, /* ?:                (right-associative) */
  PREC_OR,         /* ||               */
  PREC_AND,        /* &&               */
  PREC_EQUALITY,   /* === !==          */
  PREC_COMPARISON, /* < > <= >=        */
  PREC_TERM,       /* + -              */
  PREC_FACTOR,     /* * / %            */
  PREC_EXPONENT,   /* **  (right-assoc)*/
  PREC_UNARY,      /* ! - typeof ++ -- */
  PREC_CALL,       /* . ( )            */
} Precedence;

static void advanceToken(Parser *parser) {
  parser->previous = parser->current;
  for (;;) {
    parser->current = csLexerNext(&parser->lexer);
    /* The lexer already reported it; skip the token and keep going so the
     * parser sees a clean stream. */
    if (parser->current.type != TOKEN_ERROR) break;
  }
}

static bool check(const Parser *parser, TokenType type) {
  return parser->current.type == type;
}

static bool matchToken(Parser *parser, TokenType type) {
  if (!check(parser, type)) return false;
  advanceToken(parser);
  return true;
}

static void errorAtCurrent(Parser *parser, const char *message) {
  csDiagnosticError(parser->diag, parser->current.line, parser->current.start,
                    parser->current.length, "%s", message);
}

static void consume(Parser *parser, TokenType type, const char *message) {
  if (check(parser, type)) {
    advanceToken(parser);
    return;
  }
  errorAtCurrent(parser, message);
}

/* After an error, skip tokens until something that plausibly starts a new
 * statement, so the rest of the file still gets parsed and checked. */
static void synchronize(Parser *parser) {
  parser->diag->panicMode = false;

  /* Always consume at least one token. The token that triggered the error is
   * never a valid statement start, so returning without consuming it would
   * hand the same token back to the parser and loop forever. */
  if (!check(parser, TOKEN_EOF)) advanceToken(parser);

  while (!check(parser, TOKEN_EOF)) {
    if (parser->previous.type == TOKEN_SEMICOLON) return;

    switch (parser->current.type) {
      case TOKEN_LET:
      case TOKEN_CONST:
      case TOKEN_VAR:
      case TOKEN_FUNCTION:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_FOR:
      case TOKEN_RETURN:
        return;
      default:
        break;
    }
    advanceToken(parser);
  }
}

/* Maps a token to its binary precedence, or PREC_NONE if it is not a binary
 * operator. */
static Precedence binaryPrecedence(TokenType type) {
  switch (type) {
    case TOKEN_PIPE_PIPE:           return PREC_OR;
    case TOKEN_AMP_AMP:             return PREC_AND;
    case TOKEN_EQUAL_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL_EQUAL:    return PREC_EQUALITY;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
    case TOKEN_INSTANCEOF:          return PREC_COMPARISON;
    case TOKEN_PLUS:
    case TOKEN_MINUS:               return PREC_TERM;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:             return PREC_FACTOR;
    case TOKEN_STAR_STAR:           return PREC_EXPONENT;
    default:                        return PREC_NONE;
  }
}

static BinaryOp binaryOpFor(TokenType type) {
  switch (type) {
    case TOKEN_PLUS:                return BINARY_ADD;
    case TOKEN_MINUS:               return BINARY_SUBTRACT;
    case TOKEN_STAR:                return BINARY_MULTIPLY;
    case TOKEN_SLASH:               return BINARY_DIVIDE;
    case TOKEN_PERCENT:             return BINARY_MODULO;
    case TOKEN_STAR_STAR:           return BINARY_EXPONENT;
    case TOKEN_EQUAL_EQUAL_EQUAL:   return BINARY_EQUAL;
    case TOKEN_BANG_EQUAL_EQUAL:    return BINARY_NOT_EQUAL;
    case TOKEN_GREATER:             return BINARY_GREATER;
    case TOKEN_GREATER_EQUAL:       return BINARY_GREATER_EQUAL;
    case TOKEN_LESS:                return BINARY_LESS;
    case TOKEN_LESS_EQUAL:          return BINARY_LESS_EQUAL;
    case TOKEN_INSTANCEOF:          return BINARY_INSTANCEOF;
    default:                        return BINARY_ADD; /* unreachable */
  }
}

static AstNode *parseExpression(Parser *parser);
static AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence);
static AstNode *parsePrimary(Parser *parser);
static AstNode *parseFunction(Parser *parser, bool requireName);
static AstNode *parseSwitch(Parser *parser);
static AstNode *parseTry(Parser *parser);
static AstNode *parseDestructuring(Parser *parser, bool isObject, bool isConst);
static AstNode *parseBlock(Parser *parser);
static AstNode *finishVarDeclaration(Parser *parser, int line, const char *name,
                                     int nameLength, bool isConst);
static AstNode *parseTemplate(Parser *parser, const char *start, int length, int line);
static AstNode *parseStatement(Parser *parser);
static void synchronize(Parser *parser);
static bool parseTypeAnnotation(Parser *parser, TypeKind *type, bool *present);
static bool looksLikeArrowParams(Parser *parser);
static AstNode *finishArrow(Parser *parser, AstNode *function, int line);

/* Decodes a string literal's escape sequences into a fresh arena buffer.
 * `start`/`length` cover the lexeme including both quotes. */
static AstNode *makeStringLiteral(Parser *parser, const char *start, int length,
                                  int line) {
  const char *src = start + 1;   /* skip the opening quote */
  int srcLength = length - 2;    /* and the closing one */
  if (srcLength < 0) srcLength = 0;

  /* Decoding only ever shrinks, so the source length is a safe upper bound. */
  char *buffer = (char *)csAstArenaAlloc(parser->arena, (size_t)srcLength + 1);
  if (buffer == NULL) return NULL;

  int out = 0;
  for (int i = 0; i < srcLength; i++) {
    if (src[i] != '\\' || i + 1 >= srcLength) {
      buffer[out++] = src[i];
      continue;
    }

    i++;
    switch (src[i]) {
      case 'n':  buffer[out++] = '\n'; break;
      case 't':  buffer[out++] = '\t'; break;
      case 'r':  buffer[out++] = '\r'; break;
      case '0':  buffer[out++] = '\0'; break;
      case '\\': buffer[out++] = '\\'; break;
      case '\'': buffer[out++] = '\''; break;
      case '"':  buffer[out++] = '"';  break;
      default:
        /* Unknown escape: JS keeps the character as written. */
        buffer[out++] = src[i];
        break;
    }
  }
  buffer[out] = '\0';

  return csAstString(parser->arena, line, buffer, out);
}

static double parseNumberLiteral(const char *start, int length) {
  char buffer[64];
  int copyLength = length < (int)sizeof(buffer) - 1 ? length : (int)sizeof(buffer) - 1;
  memcpy(buffer, start, (size_t)copyLength);
  buffer[copyLength] = '\0';

  if (copyLength > 2 && buffer[0] == '0' && (buffer[1] == 'x' || buffer[1] == 'X')) {
    return (double)strtoull(buffer + 2, NULL, 16);
  }
  return strtod(buffer, NULL);
}

/* `==` and `!=` exist in the grammar so the error can name them, but they never
 * produce a node: CScript has no coercing equality. See docs/GRAMMAR.md. */
static bool rejectLooseEquality(Parser *parser) {
  if (check(parser, TOKEN_EQUAL_EQUAL)) {
    errorAtCurrent(parser,
                   "'==' is not supported because it coerces its operands; use '==='");
    return true;
  }
  if (check(parser, TOKEN_BANG_EQUAL)) {
    errorAtCurrent(parser,
                   "'!=' is not supported because it coerces its operands; use '!=='");
    return true;
  }
  return false;
}

/* Postfix `.name` and `(args)`, which bind tighter than any unary operator. */
static AstNode *parseCallSuffixes(Parser *parser, AstNode *expression) {
  for (;;) {
    if (matchToken(parser, TOKEN_DOT)) {
      int line = parser->previous.line;
      consume(parser, TOKEN_IDENTIFIER, "expected a property name after '.'");
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
static AstNode *parsePrimary(Parser *parser) {
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
      consume(parser, TOKEN_IDENTIFIER, "expected a property name after '.'");
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

  if (matchToken(parser, TOKEN_IDENTIFIER)) {
    const char *name = parser->previous.start;
    int nameLength = parser->previous.length;

    /* `x => ...` — a single parameter needs no parentheses. */
    if (check(parser, TOKEN_ARROW)) {
      advanceToken(parser);
      AstNode *arrow = csAstFunction(parser->arena, line, NULL, 0);
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

        /* Keys may be written bare or quoted; both become string constants. */
        AstNode *key;
        if (matchToken(parser, TOKEN_STRING)) {
          key = makeStringLiteral(parser, parser->previous.start,
                                  parser->previous.length, parser->previous.line);
        } else {
          consume(parser, TOKEN_IDENTIFIER, "expected a property name");
          if (parser->diag->panicMode) return NULL;
          key = csAstString(parser->arena, parser->previous.line,
                            parser->previous.start, parser->previous.length);
        }

        consume(parser, TOKEN_COLON, "expected ':' after the property name");
        if (parser->diag->panicMode) return NULL;

        AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
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

      if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
          if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
            errorAtCurrent(parser,
                           "destructuring a parameter is not supported yet; "
                           "destructure inside the body instead");
            return NULL;
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

  if (rejectLooseEquality(parser)) return NULL;

  errorAtCurrent(parser, "expected an expression");
  return NULL;
}

/* Maps a compound assignment token to the operation it expands to. */
static bool compoundAssignOp(TokenType type, BinaryOp *out) {
  switch (type) {
    case TOKEN_PLUS_EQUAL:    *out = BINARY_ADD; return true;
    case TOKEN_MINUS_EQUAL:   *out = BINARY_SUBTRACT; return true;
    case TOKEN_STAR_EQUAL:    *out = BINARY_MULTIPLY; return true;
    case TOKEN_SLASH_EQUAL:   *out = BINARY_DIVIDE; return true;
    case TOKEN_PERCENT_EQUAL: *out = BINARY_MODULO; return true;
    case TOKEN_STAR_STAR_EQUAL: *out = BINARY_EXPONENT; return true;
    default: return false;
  }
}

/* Precedence climbing: parse a primary, then keep folding in operators whose
 * precedence is at least `minPrecedence`. Binary operators here are
 * left-associative, so their right side is parsed one level tighter;
 * assignment is right-associative and so parses at its own level. */
static AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence) {
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

static AstNode *parseExpression(Parser *parser) {
  return parsePrecedence(parser, PREC_ASSIGNMENT);
}

static AstNode *parseStatement(Parser *parser);

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
static AstNode *parseTemplate(Parser *parser, const char *start, int length, int line) {
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

/* Parses `: TypeName` if present. Returns false only on a malformed one. */
static bool parseTypeAnnotation(Parser *parser, TypeKind *type, bool *present) {
  *present = false;
  *type = TYPE_ANY;
  if (!matchToken(parser, TOKEN_COLON)) return true;

  /* `null` and `undefined` are keywords, so they do not arrive as identifiers
   * even though they are perfectly good type names. */
  if (!matchToken(parser, TOKEN_NULL) && !matchToken(parser, TOKEN_UNDEFINED)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a type name after ':'");
  }
  if (parser->diag->panicMode) return false;

  if (!csTypeFromName(parser->previous.start, parser->previous.length, type)) {
    csDiagnosticError(parser->diag, parser->previous.line, parser->previous.start,
                      parser->previous.length, "unknown type '%.*s'",
                      parser->previous.length, parser->previous.start);
    return false;
  }
  *present = true;
  return true;
}

static AstNode *parseBlock(Parser *parser);

/* Looks past a '(' for the ')' that closes it and reports whether '=>' follows.
 *
 * `(a, b)` is a parameter list or a parenthesised expression, and nothing
 * before the arrow distinguishes them. Rather than backtracking the parser,
 * this scans the raw token stream with a throwaway lexer — cheap, because the
 * span is short, and it leaves the real parser's state untouched. */
static bool looksLikeArrowParams(Parser *parser) {
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
static AstNode *finishArrow(Parser *parser, AstNode *function, int line) {
  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    function->as.function.body = parseBlock(parser);
  } else {
    AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
    if (value == NULL) return NULL;
    /* `x => expr` is `x => { return expr; }`. */
    AstNode *body = csAstBlock(parser->arena, line);
    csAstProgramAdd(parser->arena, body, csAstReturn(parser->arena, line, value));
    function->as.function.body = body;
  }
  return function->as.function.body != NULL ? function : NULL;
}

/* Everything after the name: the parameter list, the return annotation and the
 * body. Shared by function declarations and class methods, which differ only
 * in how their name is introduced. */
static AstNode *parseFunctionRest(Parser *parser, int line, const char *name,
                                  int nameLength, bool isMethod) {
  AstNode *function = csAstFunction(parser->arena, line, name, nameLength);

  consume(parser, TOKEN_LEFT_PAREN,
          isMethod ? "expected '(' after the method name"
                   : "expected '(' after the function name");
  if (parser->diag->panicMode) return NULL;

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
        errorAtCurrent(parser, "destructuring a parameter is not supported yet; "
                               "destructure inside the body instead");
        return NULL;
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

  function->as.function.body = parseBlock(parser);
  if (function->as.function.body == NULL) return NULL;

  return function;
}

/* `function name(a: number, b): number { ... }`
 *
 * A named declaration binds the closure; an anonymous one is an expression. */
static AstNode *parseFunction(Parser *parser, bool requireName) {
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

static bool nameIs(const char *name, int length, const char *word) {
  int wordLength = (int)strlen(word);
  return length == wordLength && memcmp(name, word, (size_t)wordLength) == 0;
}

/* `class Name extends Base { field; field = init; constructor() {} m() {} static m() {} }`
 *
 * Members are separated by nothing at all in JavaScript, so the loop reads one
 * member at a time and decides what it was from the token after the name: a
 * '(' means a method, anything else a field. */
static AstNode *parseClass(Parser *parser) {
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

    consume(parser, TOKEN_IDENTIFIER, "expected a member name");
    if (parser->diag->panicMode) return NULL;
    const char *memberName = parser->previous.start;
    int memberLength = parser->previous.length;
    int memberLine = parser->previous.line;

    if (nameIs(memberName, memberLength, "get") || nameIs(memberName, memberLength, "set")) {
      if (check(parser, TOKEN_IDENTIFIER)) {
        errorAtCurrent(parser, "getters and setters are not supported yet");
        return NULL;
      }
    }

    if (check(parser, TOKEN_LEFT_PAREN)) {
      /* The constructor is named after its class, because that is what an
       * arity error or a stack frame should say: `Dog expects 1 argument`
       * rather than `constructor expects 1 argument`. */
      bool isConstructor = nameIs(memberName, memberLength, "constructor");
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
        csAstClassAddMember(parser->arena, node, method, isStatic);
      }
      continue;
    }

    if (isStatic) {
      errorAtCurrent(parser, "static fields are not supported yet; "
                             "assign to the class after declaring it");
      return NULL;
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
                       fieldType, annotated);
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the class body");
  if (parser->diag->panicMode) return NULL;
  return node;
}

/* `try { } catch (e) { } finally { }`
 *
 * At least one of catch and finally must be present, since `try` alone does
 * nothing. The catch binding is optional, matching modern JavaScript. */
static AstNode *parseTry(Parser *parser) {
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
static AstNode *parseSwitch(Parser *parser) {
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

/* Parses everything after a binding's name: the optional annotation, the
 * optional initialiser and the semicolon.
 *
 * Split out from parseVarDeclaration because `for (const x of xs)` and
 * `for (let i = 0; ...)` only diverge after the name, so the caller has to read
 * it before it knows which form it is looking at. */
static AstNode *finishVarDeclaration(Parser *parser, int line, const char *name,
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

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a variable declaration");
  if (parser->diag->panicMode) return NULL;

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

/* `const [a, b, ...rest] = xs;` and `const { x, y: alias, z = 1 } = o;`
 *
 * Both forms compile to the loads and stores they stand for, so nothing new
 * exists at run time. Nested patterns are not supported and say so. */
static AstNode *parseDestructuring(Parser *parser, bool isObject, bool isConst) {
  int line = parser->previous.line;
  AstNode *pattern = csAstDestructure(parser->arena, line, isObject, isConst);
  TokenType closer = isObject ? TOKEN_RIGHT_BRACE : TOKEN_RIGHT_BRACKET;

  if (!check(parser, closer)) {
    do {
      if (check(parser, closer)) break; /* a trailing comma */

      bool isRest = matchToken(parser, TOKEN_ELLIPSIS);
      if (isRest && isObject) {
        errorAtCurrent(parser, "rest properties are not supported in object patterns");
        return NULL;
      }

      if (check(parser, TOKEN_LEFT_BRACKET) || check(parser, TOKEN_LEFT_BRACE)) {
        errorAtCurrent(parser, "nested destructuring patterns are not supported yet");
        return NULL;
      }

      consume(parser, TOKEN_IDENTIFIER, "expected a name in the pattern");
      if (parser->diag->panicMode) return NULL;

      const char *key = parser->previous.start;
      int keyLength = parser->previous.length;
      const char *name = key;
      int nameLength = keyLength;

      /* `{ key: localName }` renames on the way in. */
      if (isObject && matchToken(parser, TOKEN_COLON)) {
        consume(parser, TOKEN_IDENTIFIER, "expected a name after ':'");
        if (parser->diag->panicMode) return NULL;
        name = parser->previous.start;
        nameLength = parser->previous.length;
      }

      AstNode *defaultValue = NULL;
      if (matchToken(parser, TOKEN_EQUAL)) {
        defaultValue = parsePrecedence(parser, PREC_ASSIGNMENT);
        if (defaultValue == NULL) return NULL;
      }

      csAstDestructureAdd(parser->arena, pattern, isObject ? key : NULL, keyLength,
                          name, nameLength, defaultValue, isRest);

      if (isRest) break; /* nothing may follow a rest element */
    } while (matchToken(parser, TOKEN_COMMA));
  }

  consume(parser, closer, isObject ? "expected '}' to close the pattern"
                                   : "expected ']' to close the pattern");
  consume(parser, TOKEN_EQUAL, "a destructuring declaration needs an initialiser");
  if (parser->diag->panicMode) return NULL;

  pattern->as.destructure.initializer = parsePrecedence(parser, PREC_ASSIGNMENT);
  if (pattern->as.destructure.initializer == NULL) return NULL;

  consume(parser, TOKEN_SEMICOLON, "expected ';' after the declaration");
  return parser->diag->panicMode ? NULL : pattern;
}

/* `let x = 1;` / `const y = 2;` — `var` is rejected in parseStatement. */
static AstNode *parseVarDeclaration(Parser *parser, bool isConst) {
  int line = parser->previous.line;
  (void)line;

  /* A pattern rather than a name means this is a destructuring declaration. */
  if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
    return parseDestructuring(parser, false, isConst);
  }
  if (matchToken(parser, TOKEN_LEFT_BRACE)) {
    return parseDestructuring(parser, true, isConst);
  }

  consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
  if (parser->diag->panicMode) return NULL;

  return finishVarDeclaration(parser, line, parser->previous.start,
                              parser->previous.length, isConst);
}

static AstNode *parseBlock(Parser *parser) {
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

static AstNode *parseIf(Parser *parser) {
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

static AstNode *parseWhile(Parser *parser) {
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
static AstNode *parseFor(Parser *parser) {
  int line = parser->previous.line;
  consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'for'");
  if (parser->diag->panicMode) return NULL;

  AstNode *initializer = NULL;
  if (matchToken(parser, TOKEN_SEMICOLON)) {
    initializer = NULL;
  } else if (check(parser, TOKEN_LET) || check(parser, TOKEN_CONST)) {
    bool isConst = check(parser, TOKEN_CONST);
    advanceToken(parser);

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
    if (isForOf) {
      advanceToken(parser);
      AstNode *iterable = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the iterable");
      if (iterable == NULL || parser->diag->panicMode) return NULL;

      AstNode *body = parseStatement(parser);
      if (body == NULL) return NULL;
      return csAstForOf(parser->arena, line, bindingName, bindingLength, isConst,
                        iterable, body);
    }

    initializer = finishVarDeclaration(parser, line, bindingName, bindingLength,
                                       isConst);
    if (initializer == NULL) return NULL;
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
static const char *notImplementedMessage(TokenType type) {
  switch (type) {
    default:
      return NULL;
  }
}

static AstNode *parseStatement(Parser *parser) {
  int line = parser->current.line;

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
    consume(parser, TOKEN_SEMICOLON, "expected ';' after 'break'");
    if (parser->diag->panicMode) return NULL;
    return csAstBreak(parser->arena, breakLine);
  }

  if (matchToken(parser, TOKEN_CONTINUE)) {
    int continueLine = parser->previous.line;
    consume(parser, TOKEN_SEMICOLON, "expected ';' after 'continue'");
    if (parser->diag->panicMode) return NULL;
    return csAstContinue(parser->arena, continueLine);
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

  if (matchToken(parser, TOKEN_CLASS)) return parseClass(parser);

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

AstNode *csParse(const char *source, AstArena *arena, Diagnostics *diag) {
  Parser parser;
  parser.arena = arena;
  parser.diag = diag;
  csLexerInit(&parser.lexer, source, diag);

  /* Prime `current`; `previous` is not read until after the first advance. */
  parser.previous.type = TOKEN_EOF;
  parser.previous.start = source;
  parser.previous.length = 0;
  parser.previous.line = 1;
  advanceToken(&parser);

  AstNode *program = csAstProgram(arena, 1);
  if (program == NULL) return NULL;

  /* Past a certain point extra messages stop being informative and start
   * burying the first one, which is the one that usually matters. */
  const int maxReportedErrors = 20;

  while (!check(&parser, TOKEN_EOF)) {
    AstNode *statement = parseStatement(&parser);
    if (statement != NULL) csAstProgramAdd(arena, program, statement);
    if (diag->panicMode) synchronize(&parser);

    if (diag->errorCount >= maxReportedErrors) {
      fprintf(stderr, "%s: too many errors; stopping after %d\n", diag->sourceName,
              maxReportedErrors);
      break;
    }
  }

  return csDiagnosticsFailed(diag) ? NULL : program;
}
