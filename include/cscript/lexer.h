/* lexer.h — turns source text into a token stream.
 *
 * Tokens borrow from the source buffer rather than copying: `start`/`length`
 * point into it. The buffer must outlive every token, which the driver
 * guarantees by freeing the source only after compilation finishes.
 */
#ifndef CSCRIPT_LEXER_H
#define CSCRIPT_LEXER_H

#include "cscript/common.h"
#include "cscript/diagnostic.h"

typedef enum {
  /* Single-character punctuation. */
  TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
  TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
  TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
  TOKEN_COMMA, TOKEN_DOT, TOKEN_ELLIPSIS, TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_QUESTION,
  TOKEN_QUESTION_DOT, /* optional chaining: ?. and, with a lookahead, ?.[ ?.( */

  /* Arithmetic. */
  TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
  TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,
  TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL,
  TOKEN_PERCENT_EQUAL,
  TOKEN_STAR_STAR, TOKEN_STAR_STAR_EQUAL,
  TOKEN_ARROW,

  /* Comparison and assignment. */
  TOKEN_BANG, TOKEN_BANG_EQUAL, TOKEN_BANG_EQUAL_EQUAL,
  TOKEN_EQUAL, TOKEN_EQUAL_EQUAL, TOKEN_EQUAL_EQUAL_EQUAL,
  TOKEN_GREATER, TOKEN_GREATER_EQUAL,
  TOKEN_LESS, TOKEN_LESS_EQUAL,

  /* Logical. */
  TOKEN_AMP_AMP, TOKEN_PIPE_PIPE, TOKEN_QUESTION_QUESTION,
  /* Logical assignment. Short-circuiting, so these are not sugar for the
   * matching binary operator: `a ||= b` must not read `a`'s setter twice, and
   * `a ??= b` must not evaluate `b` when `a` is already there. */
  TOKEN_AMP_AMP_EQUAL, TOKEN_PIPE_PIPE_EQUAL, TOKEN_QUESTION_QUESTION_EQUAL,

  /* Literals. */
  TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER, TOKEN_TEMPLATE, TOKEN_REGEX,

  /* Keywords. */
  TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL, TOKEN_UNDEFINED,
  TOKEN_LET, TOKEN_CONST, TOKEN_VAR, TOKEN_FUNCTION, TOKEN_RETURN,
  TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR,
  TOKEN_TYPEOF,
  TOKEN_SWITCH, TOKEN_CASE, TOKEN_DEFAULT, TOKEN_OF,
  TOKEN_BREAK, TOKEN_CONTINUE,
  TOKEN_TRY, TOKEN_CATCH, TOKEN_FINALLY, TOKEN_THROW,
  TOKEN_CLASS, TOKEN_EXTENDS, TOKEN_NEW, TOKEN_THIS, TOKEN_SUPER,
  TOKEN_STATIC, TOKEN_INSTANCEOF,
  TOKEN_IMPORT, TOKEN_EXPORT, TOKEN_AWAIT, TOKEN_DO, TOKEN_IN, TOKEN_DELETE,

  TOKEN_ERROR,
  TOKEN_EOF,
} TokenType;

typedef struct {
  TokenType type;
  const char *start; /* points into the source buffer */
  int length;
  int line;
} Token;

typedef struct {
  const char *start;   /* start of the token being scanned */
  const char *current; /* read cursor */
  int line;
  Diagnostics *diag;
} Lexer;

void csLexerInit(Lexer *lexer, const char *source, Diagnostics *diag);
Token csLexerNext(Lexer *lexer);

/* Rescans a `/` as the start of a regular expression literal.
 *
 * Whether `/` opens a regex or divides is not decidable by the lexer: `a / b`
 * and `/a/.test(b)` differ only in what came before. So the lexer does not
 * guess — the *parser* asks, at the one point it knows a value is expected,
 * and this continues from just after the slash it was handed. */
Token csLexerScanRegex(Lexer *lexer);

/* Human-readable token name, for debug dumps and error messages. */
const char *csTokenTypeName(TokenType type);

/* Dumps the whole token stream to stdout. Consumes a lexer of its own. */
void csLexerDumpTokens(const char *source, Diagnostics *diag);

#endif /* CSCRIPT_LEXER_H */
