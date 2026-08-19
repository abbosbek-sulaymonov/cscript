#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/parser.h"

/* Binding powers, lowest binds loosest. Mirrors JS operator precedence for the
 * operators that exist today; new levels slot in without touching the parser
 * functions, because binary parsing is driven by this table. */
typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT, /* = += -= *= /= %=  (right-associative) */
  PREC_OR,         /* ||               */
  PREC_AND,        /* &&               */
  PREC_EQUALITY,   /* === !==          */
  PREC_COMPARISON, /* < > <= >=        */
  PREC_TERM,       /* + -              */
  PREC_FACTOR,     /* * / %            */
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
    case TOKEN_EQUAL_EQUAL_EQUAL:   return BINARY_EQUAL;
    case TOKEN_BANG_EQUAL_EQUAL:    return BINARY_NOT_EQUAL;
    case TOKEN_GREATER:             return BINARY_GREATER;
    case TOKEN_GREATER_EQUAL:       return BINARY_GREATER_EQUAL;
    case TOKEN_LESS:                return BINARY_LESS;
    case TOKEN_LESS_EQUAL:          return BINARY_LESS_EQUAL;
    default:                        return BINARY_ADD; /* unreachable */
  }
}

static AstNode *parseExpression(Parser *parser);
static AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence);
static AstNode *parsePrimary(Parser *parser);

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

    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      int line = parser->previous.line;
      AstNode *call = csAstCall(parser->arena, line, expression);
      if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
          AstNode *argument = parsePrecedence(parser, PREC_ASSIGNMENT);
          if (argument == NULL) return NULL;
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
  if (matchToken(parser, TOKEN_TRUE)) {
    return csAstBool(parser->arena, line, true);
  }
  if (matchToken(parser, TOKEN_FALSE)) {
    return csAstBool(parser->arena, line, false);
  }
  if (matchToken(parser, TOKEN_NULL)) return csAstNull(parser->arena, line);
  if (matchToken(parser, TOKEN_UNDEFINED)) return csAstUndefined(parser->arena, line);

  if (matchToken(parser, TOKEN_IDENTIFIER)) {
    AstNode *identifier = csAstIdentifier(parser->arena, line, parser->previous.start,
                                          parser->previous.length);
    return parseCallSuffixes(parser, identifier);
  }

  if (matchToken(parser, TOKEN_LEFT_PAREN)) {
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

      if (left->type != AST_IDENTIFIER) {
        csDiagnosticError(parser->diag, line, NULL, 0,
                          "the left side of an assignment must be a variable");
        return NULL;
      }

      AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (value == NULL) return NULL;
      if (isCompound) value = csAstBinary(parser->arena, line, compound, left, value);

      left = csAstAssign(parser->arena, line, left, value);
      continue;
    }

    if (rejectLooseEquality(parser)) return NULL;

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
  return parsePrecedence(parser, PREC_ASSIGNMENT);
}

static AstNode *parseStatement(Parser *parser);

/* `let x = 1;` / `const y = 2;` — `var` is rejected in parseStatement. */
static AstNode *parseVarDeclaration(Parser *parser, bool isConst) {
  int line = parser->previous.line;

  consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
  if (parser->diag->panicMode) return NULL;

  const char *name = parser->previous.start;
  int nameLength = parser->previous.length;

  /* Optional TypeScript-style annotation. Leaving it off is not the same as
   * writing `: any` — an unannotated declaration takes its initialiser's type,
   * so most code is checked without being annotated. */
  TypeKind declaredType = TYPE_ANY;
  bool hasAnnotation = false;
  if (matchToken(parser, TOKEN_COLON)) {
    /* `null` and `undefined` are keywords, so they do not arrive as
     * identifiers even though they are perfectly good type names. */
    if (!matchToken(parser, TOKEN_NULL) && !matchToken(parser, TOKEN_UNDEFINED)) {
      consume(parser, TOKEN_IDENTIFIER, "expected a type name after ':'");
    }
    if (parser->diag->panicMode) return NULL;

    if (!csTypeFromName(parser->previous.start, parser->previous.length,
                        &declaredType)) {
      csDiagnosticError(parser->diag, parser->previous.line, parser->previous.start,
                        parser->previous.length, "unknown type '%.*s'",
                        parser->previous.length, parser->previous.start);
      return NULL;
    }
    hasAnnotation = true;
  }

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

  return csAstVarDecl(parser->arena, line, name, nameLength, initializer, isConst,
                      declaredType, hasAnnotation);
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
  } else if (matchToken(parser, TOKEN_LET)) {
    initializer = parseVarDeclaration(parser, false);
    if (initializer == NULL) return NULL;
  } else if (matchToken(parser, TOKEN_CONST)) {
    initializer = parseVarDeclaration(parser, true);
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
    case TOKEN_FUNCTION:
    case TOKEN_RETURN:
      return "functions are not implemented yet (milestone 3)";
    case TOKEN_LEFT_BRACKET:
      return "arrays are not implemented yet (milestone 4)";
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
