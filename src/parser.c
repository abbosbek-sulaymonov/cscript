#include <stdlib.h>
#include <string.h>

#include "cscript/parser.h"

/* Binding powers, lowest binds loosest. Mirrors JS operator precedence for the
 * operators that exist today; new levels slot in without touching the parser
 * functions, because binary parsing is driven by this table. */
typedef enum {
  PREC_NONE,
  PREC_OR,         /* ||           */
  PREC_AND,        /* &&           */
  PREC_EQUALITY,   /* == != === !==*/
  PREC_COMPARISON, /* < > <= >=    */
  PREC_TERM,       /* + -          */
  PREC_FACTOR,     /* * / %        */
  PREC_UNARY,      /* ! - typeof   */
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
      case TOKEN_PRINT:
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
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_EQUAL_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL_EQUAL:    return PREC_EQUALITY;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:       return PREC_COMPARISON;
    case TOKEN_PLUS:
    case TOKEN_MINUS:               return PREC_TERM;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:             return PREC_FACTOR;
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
    case TOKEN_EQUAL_EQUAL:         return BINARY_EQUAL;
    case TOKEN_BANG_EQUAL:          return BINARY_NOT_EQUAL;
    case TOKEN_EQUAL_EQUAL_EQUAL:   return BINARY_STRICT_EQUAL;
    case TOKEN_BANG_EQUAL_EQUAL:    return BINARY_STRICT_NOT_EQUAL;
    case TOKEN_GREATER:             return BINARY_GREATER;
    case TOKEN_GREATER_EQUAL:       return BINARY_GREATER_EQUAL;
    case TOKEN_LESS:                return BINARY_LESS;
    case TOKEN_LESS_EQUAL:          return BINARY_LESS_EQUAL;
    default:                        return BINARY_ADD; /* unreachable */
  }
}

static AstNode *parseExpression(Parser *parser);
static AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence);

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

/* Literals, identifiers, prefix operators and parenthesised groups. */
static AstNode *parsePrimary(Parser *parser) {
  int line = parser->current.line;

  if (matchToken(parser, TOKEN_NUMBER)) {
    return csAstNumber(parser->arena, line,
                       parseNumberLiteral(parser->previous.start, parser->previous.length));
  }
  if (matchToken(parser, TOKEN_STRING)) {
    return makeStringLiteral(parser, parser->previous.start, parser->previous.length, line);
  }
  if (matchToken(parser, TOKEN_TRUE))  return csAstBool(parser->arena, line, true);
  if (matchToken(parser, TOKEN_FALSE)) return csAstBool(parser->arena, line, false);
  if (matchToken(parser, TOKEN_NULL))  return csAstNull(parser->arena, line);
  if (matchToken(parser, TOKEN_UNDEFINED)) return csAstUndefined(parser->arena, line);

  if (matchToken(parser, TOKEN_LEFT_PAREN)) {
    AstNode *inner = parseExpression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after expression");
    if (inner == NULL) return NULL;
    return csAstGrouping(parser->arena, line, inner);
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
  if (matchToken(parser, TOKEN_PLUS)) {
    /* Unary plus is ToNumber; expressed as `x - 0` would change NaN handling,
     * so it is rejected until numeric coercion has an opcode of its own. */
    errorAtCurrent(parser, "unary '+' is not supported yet");
    return NULL;
  }

  if (check(parser, TOKEN_IDENTIFIER)) {
    csDiagnosticError(parser->diag, parser->current.line, parser->current.start,
                      parser->current.length,
                      "variables are not implemented yet (milestone 2)");
    advanceToken(parser);
    return NULL;
  }

  errorAtCurrent(parser, "expected an expression");
  return NULL;
}

/* Precedence climbing: parse a primary, then keep folding in operators whose
 * precedence is at least `minPrecedence`. All binary operators here are
 * left-associative, so the right side is parsed one level tighter. */
static AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence) {
  AstNode *left = parsePrimary(parser);
  if (left == NULL) return NULL;

  for (;;) {
    Precedence precedence = binaryPrecedence(parser->current.type);
    if (precedence == PREC_NONE || precedence < minPrecedence) break;

    TokenType operatorType = parser->current.type;
    int line = parser->current.line;
    advanceToken(parser);

    AstNode *right = parsePrecedence(parser, (Precedence)(precedence + 1));
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
  return parsePrecedence(parser, PREC_OR);
}

/* Keywords the lexer already recognises but no stage implements yet. Reporting
 * them by name beats "expected an expression", which tells the reader nothing
 * about why a perfectly ordinary line was rejected. */
static const char *notImplementedMessage(TokenType type) {
  switch (type) {
    case TOKEN_LET:
    case TOKEN_CONST:
    case TOKEN_VAR:
      return "variables are not implemented yet (milestone 2)";
    case TOKEN_IF:
    case TOKEN_ELSE:
    case TOKEN_WHILE:
    case TOKEN_FOR:
    case TOKEN_LEFT_BRACE:
      return "control flow and blocks are not implemented yet (milestone 3)";
    case TOKEN_FUNCTION:
    case TOKEN_RETURN:
      return "functions are not implemented yet (milestone 4)";
    case TOKEN_LEFT_BRACKET:
      return "arrays are not implemented yet (milestone 5)";
    default:
      return NULL;
  }
}

static AstNode *parseStatement(Parser *parser) {
  int line = parser->current.line;

  const char *notImplemented = notImplementedMessage(parser->current.type);
  if (notImplemented != NULL) {
    errorAtCurrent(parser, notImplemented);
    return NULL;
  }

  if (matchToken(parser, TOKEN_PRINT)) {
    AstNode *value = parseExpression(parser);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after print statement");
    if (value == NULL) return NULL;
    return csAstPrintStmt(parser->arena, line, value);
  }

  AstNode *expression = parseExpression(parser);
  consume(parser, TOKEN_SEMICOLON, "expected ';' after expression statement");
  if (expression == NULL) return NULL;
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

  while (!check(&parser, TOKEN_EOF)) {
    AstNode *statement = parseStatement(&parser);
    if (statement != NULL) {
      csAstProgramAdd(arena, program, statement);
    }
    if (diag->panicMode) synchronize(&parser);
  }

  return csDiagnosticsFailed(diag) ? NULL : program;
}
