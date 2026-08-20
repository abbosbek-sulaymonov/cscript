#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/parser.h"
#include "parser_internal.h"



void advanceToken(Parser *parser) {
  parser->previous = parser->current;
  for (;;) {
    parser->current = csLexerNext(&parser->lexer);
    /* The lexer already reported it; skip the token and keep going so the
     * parser sees a clean stream. */
    if (parser->current.type != TOKEN_ERROR) break;
  }
}

bool check(const Parser *parser, TokenType type) {
  return parser->current.type == type;
}

bool matchToken(Parser *parser, TokenType type) {
  if (!check(parser, type)) return false;
  advanceToken(parser);
  return true;
}

void errorAtCurrent(Parser *parser, const char *message) {
  csDiagnosticError(parser->diag, parser->current.line, parser->current.start,
                    parser->current.length, "%s", message);
}

void consume(Parser *parser, TokenType type, const char *message) {
  if (check(parser, type)) {
    advanceToken(parser);
    return;
  }
  errorAtCurrent(parser, message);
}

/* After an error, skip tokens until something that plausibly starts a new
 * statement, so the rest of the file still gets parsed and checked. */
void synchronize(Parser *parser) {
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
Precedence binaryPrecedence(TokenType type) {
  switch (type) {
    case TOKEN_PIPE_PIPE:           return PREC_OR;
    /* `??` sits on the same tier as `||`. JavaScript then forbids mixing the
     * two without parentheses rather than picking a winner, because either
     * choice would be a coin-flip for the reader; parsePrecedence enforces
     * that separately. */
    case TOKEN_QUESTION_QUESTION:   return PREC_OR;
    case TOKEN_AMP_AMP:             return PREC_AND;
    case TOKEN_EQUAL_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL_EQUAL:    return PREC_EQUALITY;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
    case TOKEN_INSTANCEOF:          return PREC_COMPARISON;
    case TOKEN_IN:                  return PREC_COMPARISON;
    case TOKEN_PLUS:
    case TOKEN_MINUS:               return PREC_TERM;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:             return PREC_FACTOR;
    case TOKEN_STAR_STAR:           return PREC_EXPONENT;
    default:                        return PREC_NONE;
  }
}

BinaryOp binaryOpFor(TokenType type) {
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
    case TOKEN_IN:                  return BINARY_IN;
    default:                        return BINARY_ADD; /* unreachable */
  }
}


/* Decodes a string literal's escape sequences into a fresh arena buffer.
 * `start`/`length` cover the lexeme including both quotes. */
AstNode *makeStringLiteral(Parser *parser, const char *start, int length,
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

double parseNumberLiteral(const char *start, int length) {
  char buffer[64];
  int limit = length < (int)sizeof(buffer) - 1 ? length : (int)sizeof(buffer) - 1;

  /* `1_000_000`. The separators are dropped here rather than in the lexer, so
   * the token still spans exactly what was written and an error can point at
   * it. */
  int copyLength = 0;
  for (int i = 0; i < limit; i++) {
    if (start[i] != '_') buffer[copyLength++] = start[i];
  }
  buffer[copyLength] = '\0';

  if (copyLength > 2 && buffer[0] == '0' && (buffer[1] == 'x' || buffer[1] == 'X')) {
    return (double)strtoull(buffer + 2, NULL, 16);
  }
  return strtod(buffer, NULL);
}

/* `==` and `!=` exist in the grammar so the error can name them, but they never
 * produce a node: CScript has no coercing equality. See docs/GRAMMAR.md. */
/* `x in o`. The `in` keyword exists for `for...in`; used as an operator it
 * would need a form of property lookup CScript does not have. */
bool rejectLooseEquality(Parser *parser) {
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

/* A property name may be any identifier *or* any keyword: `p.catch`, `x.of`
 * and `{ default: 1 }` are all ordinary names in JavaScript, and reserving
 * them only where they cannot be reserved would be a gratuitous difference.
 * Every keyword's lexeme starts with a letter, so that is the whole test. */
bool consumePropertyName(Parser *parser, const char *message) {
  /* `#field` is a property name wherever one is expected, and nowhere else. */
  if (parser->current.type == TOKEN_PRIVATE_NAME) {
    advanceToken(parser);
    return true;
  }

  bool wordLike = parser->current.length > 0 &&
                  ((parser->current.start[0] >= 'a' && parser->current.start[0] <= 'z') ||
                   (parser->current.start[0] >= 'A' && parser->current.start[0] <= 'Z') ||
                   parser->current.start[0] == '_' || parser->current.start[0] == '$');
  if (!wordLike || parser->current.type == TOKEN_STRING ||
      parser->current.type == TOKEN_NUMBER) {
    errorAtCurrent(parser, message);
    return false;
  }
  advanceToken(parser);
  return true;
}

/* `async` and `from` are ordinary identifiers everywhere else, so they are
 * matched by text rather than reserved. */
bool nameIsWord(const char *name, int length, const char *word) {
  int wordLength = (int)strlen(word);
  return length == wordLength && memcmp(name, word, (size_t)wordLength) == 0;
}

bool checkWord(Parser *parser, const char *word) {
  return check(parser, TOKEN_IDENTIFIER) &&
         nameIsWord(parser->current.start, parser->current.length, word);
}

/* Whether the token after the current one opens a function. Used to tell
 * `async function f() {}` from a variable that happens to be called `async`. */
bool nextStartsFunction(Parser *parser) {
  Lexer probe = parser->lexer;
  Token next = csLexerNext(&probe);
  return next.type == TOKEN_FUNCTION;
}

/* Whether what follows `async` could be an arrow's parameter list. */
bool nextStartsArrowParams(Parser *parser) {
  Lexer probe = parser->lexer;
  Token next = csLexerNext(&probe);
  return next.type == TOKEN_LEFT_PAREN || next.type == TOKEN_IDENTIFIER;
}



/* Maps a compound assignment token to the operation it expands to. */
/* `&&= ||= ??=`. Not compound assignment: these short-circuit, so the right
 * side is not evaluated and no store happens when the left side already
 * decides the answer. */
bool logicalAssignKind(TokenType type, AssignKind *out) {
  switch (type) {
    case TOKEN_AMP_AMP_EQUAL:           *out = ASSIGN_AND; return true;
    case TOKEN_PIPE_PIPE_EQUAL:         *out = ASSIGN_OR; return true;
    case TOKEN_QUESTION_QUESTION_EQUAL: *out = ASSIGN_NULLISH; return true;
    default: return false;
  }
}

bool compoundAssignOp(TokenType type, BinaryOp *out) {
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





/* Parses `: TypeName` if present. Returns false only on a malformed one. */
bool parseTypeAnnotation(Parser *parser, TypeKind *type, bool *present) {
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






bool nameIs(const char *name, int length, const char *word) {
  int wordLength = (int)strlen(word);
  return length == wordLength && memcmp(name, word, (size_t)wordLength) == 0;
}


/* True when the current token is the given contextual keyword. */
bool checkContextual(Parser *parser, const char *word) {
  return checkWord(parser, word);
}

bool matchContextual(Parser *parser, const char *word) {
  if (!checkContextual(parser, word)) return false;
  advanceToken(parser);
  return true;
}








/* `const [a, b, ...rest] = xs;` and `const { x, y: alias, z = 1 } = o;`
 *
 * Both forms compile to the loads and stores they stand for, so nothing new
 * exists at run time. Nested patterns are not supported and say so. */


/* `let x = 1;` / `const y = 2;` — `var` is rejected in parseStatement. */








AstNode *csParse(const char *source, AstArena *arena, Diagnostics *diag) {
  Parser parser;
  parser.arena = arena;
  parser.diag = diag;
  parser.pendingAsync = false;
  /* One, not zero: the top level of a file may await, and a plain function
   * nested inside it may not — which is already the rule, because every
   * non-async function body resets this to zero on the way in. */
  parser.asyncDepth = 1;
  parser.inGenerator = false;
  parser.pendingGenerator = false;
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
