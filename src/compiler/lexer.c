/* lexer.c — source text to tokens.
 *
 * One token at a time, with no buffering and no backtracking — except for one
 * thing the lexer genuinely cannot decide alone: whether a `/` opens a regular
 * expression or divides. That is settled by the parser, which asks for a
 * rescan when it knows. The keyword table and the token names are in
 * lexer_keyword.c.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/lexer.h"

#include "compiler/lexer_internal.h"

#include <stdio.h>
#include <string.h>

#include "cscript/lexer.h"

void csLexerInit(Lexer *lexer, const char *source, Diagnostics *diag) {
  lexer->start = source;
  lexer->current = source;
  lexer->line = 1;
  lexer->diag = diag;
}

static bool isAtEnd(const Lexer *lexer) { return *lexer->current == '\0'; }

static char advance(Lexer *lexer) { return *lexer->current++; }

static char peek(const Lexer *lexer) { return *lexer->current; }

static char peekNext(const Lexer *lexer) {
  return isAtEnd(lexer) ? '\0' : lexer->current[1];
}

static bool match(Lexer *lexer, char expected) {
  if (isAtEnd(lexer) || *lexer->current != expected) return false;
  lexer->current++;
  return true;
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool isHexDigit(char c) {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* JS identifiers also allow $ and _ as leading characters. */
static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static Token makeToken(const Lexer *lexer, TokenType type) {
  Token token;
  token.type = type;
  token.start = lexer->start;
  token.length = (int)(lexer->current - lexer->start);
  token.line = lexer->line;
  return token;
}

static Token errorToken(Lexer *lexer, const char *message) {
  csDiagnosticError(lexer->diag, lexer->line, lexer->start,
                    (int)(lexer->current - lexer->start), "%s", message);
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = lexer->line;
  return token;
}

static void skipWhitespaceAndComments(Lexer *lexer) {
  for (;;) {
    char c = peek(lexer);
    switch (c) {
      case ' ':
      case '\r':
      case '\t':
        advance(lexer);
        break;
      case '\n':
        lexer->line++;
        advance(lexer);
        break;
      case '/':
        if (peekNext(lexer) == '/') {
          while (peek(lexer) != '\n' && !isAtEnd(lexer)) advance(lexer);
        } else if (peekNext(lexer) == '*') {
          advance(lexer); /* '/' */
          advance(lexer); /* '*' */
          while (!isAtEnd(lexer) && !(peek(lexer) == '*' && peekNext(lexer) == '/')) {
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
          }
          if (!isAtEnd(lexer)) {
            advance(lexer); /* '*' */
            advance(lexer); /* '/' */
          }
        } else {
          return; /* a division operator, not a comment */
        }
        break;
      default:
        return;
    }
  }
}

static Token identifier(Lexer *lexer) {
  while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
  return makeToken(lexer, csLexerIdentifierType(lexer));
}

/* A digit, or the `_` that may sit between two of them. The separator is only
 * ever punctuation: parseNumberLiteral drops it before anything reads the
 * value, so `1_000` and `1000` are the same token in every other respect. */
static bool isDigitPart(char c) { return isDigit(c) || c == '_'; }
static bool isHexPart(char c) { return isHexDigit(c) || c == '_'; }

static Token number(Lexer *lexer) {
  /* Hex literals: 0x1F. */
  if (lexer->current[-1] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
    advance(lexer);
    if (!isHexDigit(peek(lexer))) return errorToken(lexer, "expected hex digits after '0x'");
    while (isHexPart(peek(lexer))) advance(lexer);
    if (peek(lexer) == 'n') {
      advance(lexer);
      return makeToken(lexer, TOKEN_BIGINT);
    }
    return makeToken(lexer, TOKEN_NUMBER);
  }

  while (isDigitPart(peek(lexer))) advance(lexer);

  /* `123n` is a BigInt. The suffix is only legal on a whole decimal or hex
   * literal, so it is looked for before the fraction and the exponent — and
   * `1.5n` therefore lexes as `1.5` followed by an identifier, which is the
   * syntax error JavaScript reports for it too. */
  if (peek(lexer) == 'n') {
    advance(lexer);
    return makeToken(lexer, TOKEN_BIGINT);
  }

  if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
    advance(lexer);
    while (isDigitPart(peek(lexer))) advance(lexer);
  }

  /* Exponent: 1e10, 2.5E-3. */
  if (peek(lexer) == 'e' || peek(lexer) == 'E') {
    const char *rewind = lexer->current;
    advance(lexer);
    if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
    if (isDigit(peek(lexer))) {
      while (isDigitPart(peek(lexer))) advance(lexer);
    } else {
      lexer->current = rewind; /* not an exponent after all */
    }
  }

  return makeToken(lexer, TOKEN_NUMBER);
}

/* Scans a whole template literal, backticks included.
 *
 * Interpolations are not tokenised here: the parser re-lexes each `${...}`
 * separately. Doing it that way keeps the main lexer free of the mode stack a
 * streaming implementation would need, at the cost of one extra pass over
 * text that is usually a few characters long. Brace depth is still tracked, so
 * an interpolation containing an object literal does not end the template
 * early. */
static Token templateLiteral(Lexer *lexer) {
  int braceDepth = 0;

  while (!isAtEnd(lexer)) {
    char c = peek(lexer);

    if (c == '\\' && peekNext(lexer) != '\0') {
      advance(lexer);
      advance(lexer);
      continue;
    }
    if (c == '\n') lexer->line++;

    if (braceDepth == 0 && c == '`') {
      advance(lexer);
      return makeToken(lexer, TOKEN_TEMPLATE);
    }
    if (c == '$' && peekNext(lexer) == '{') {
      advance(lexer);
      advance(lexer);
      braceDepth++;
      continue;
    }
    if (braceDepth > 0) {
      if (c == '{') braceDepth++;
      if (c == '}') braceDepth--;
    }
    advance(lexer);
  }

  return errorToken(lexer, "unterminated template literal");
}

static Token string(Lexer *lexer, char quote) {
  while (!isAtEnd(lexer) && peek(lexer) != quote) {
    if (peek(lexer) == '\n') {
      return errorToken(lexer, "unterminated string literal");
    }
    if (peek(lexer) == '\\' && peekNext(lexer) != '\0') {
      advance(lexer); /* consume the backslash so \" does not end the string */
    }
    advance(lexer);
  }

  if (isAtEnd(lexer)) return errorToken(lexer, "unterminated string literal");

  advance(lexer); /* closing quote */
  return makeToken(lexer, TOKEN_STRING);
}

Token csLexerScanRegex(Lexer *lexer) {
  /* `lexer->current` is just past the opening slash, which the parser has
   * already seen. The token produced covers the whole literal, slashes and
   * flags included, so the parser can hand the text straight on. */
  lexer->start = lexer->current - 1;

  bool inClass = false; /* an unescaped `/` inside `[...]` is a literal */
  for (;;) {
    if (isAtEnd(lexer) || peek(lexer) == '\n') {
      return errorToken(lexer, "unterminated regular expression");
    }
    char c = advance(lexer);
    if (c == '\\') {
      if (isAtEnd(lexer)) return errorToken(lexer, "unterminated regular expression");
      advance(lexer);
      continue;
    }
    if (c == '[') inClass = true;
    else if (c == ']') inClass = false;
    else if (c == '/' && !inClass) break;
  }

  while (isAlpha(peek(lexer))) advance(lexer);
  return makeToken(lexer, TOKEN_REGEX);
}

Token csLexerNext(Lexer *lexer) {
  skipWhitespaceAndComments(lexer);
  lexer->start = lexer->current;

  if (isAtEnd(lexer)) return makeToken(lexer, TOKEN_EOF);

  char c = advance(lexer);

  if (isAlpha(c)) return identifier(lexer);

  /* `#name` is one token, `#` and all. Carrying the hash into the name is what
   * keeps a private field out of reach: no other syntax can produce a property
   * name that starts with one. */
  if (c == '#' && isAlpha(peek(lexer))) {
    while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
    return makeToken(lexer, TOKEN_PRIVATE_NAME);
  }
  if (isDigit(c)) return number(lexer);

  switch (c) {
    case '(': return makeToken(lexer, TOKEN_LEFT_PAREN);
    case ')': return makeToken(lexer, TOKEN_RIGHT_PAREN);
    case '{': return makeToken(lexer, TOKEN_LEFT_BRACE);
    case '}': return makeToken(lexer, TOKEN_RIGHT_BRACE);
    case '[': return makeToken(lexer, TOKEN_LEFT_BRACKET);
    case ']': return makeToken(lexer, TOKEN_RIGHT_BRACKET);
    case ',': return makeToken(lexer, TOKEN_COMMA);
    case '.':
      /* `...` for spread and rest; a lone '.' is property access. */
      if (peek(lexer) == '.' && peekNext(lexer) == '.') {
        advance(lexer);
        advance(lexer);
        return makeToken(lexer, TOKEN_ELLIPSIS);
      }
      return makeToken(lexer, TOKEN_DOT);
    case ';': return makeToken(lexer, TOKEN_SEMICOLON);
    case ':': return makeToken(lexer, TOKEN_COLON);
    case '?':
      if (match(lexer, '?')) {
        if (match(lexer, '=')) return makeToken(lexer, TOKEN_QUESTION_QUESTION_EQUAL);
        return makeToken(lexer, TOKEN_QUESTION_QUESTION);
      }
      /* `?.` only when a digit does not follow, so the conditional in
       * `x ? .5 : .2` still lexes as a question mark and a number. This is
       * the one place JavaScript's grammar needs two characters of
       * lookahead, and it is why the standard spells the rule out. */
      if (peek(lexer) == '.' && !isDigit(peekNext(lexer))) {
        advance(lexer);
        return makeToken(lexer, TOKEN_QUESTION_DOT);
      }
      return makeToken(lexer, TOKEN_QUESTION);
    case '+':
      if (match(lexer, '+')) return makeToken(lexer, TOKEN_PLUS_PLUS);
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_PLUS_EQUAL);
      return makeToken(lexer, TOKEN_PLUS);
    case '-':
      if (match(lexer, '-')) return makeToken(lexer, TOKEN_MINUS_MINUS);
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_MINUS_EQUAL);
      return makeToken(lexer, TOKEN_MINUS);
    case '*':
      if (match(lexer, '*')) {
        /* ** and **=, checked before the single star so they win. */
        if (match(lexer, '=')) return makeToken(lexer, TOKEN_STAR_STAR_EQUAL);
        return makeToken(lexer, TOKEN_STAR_STAR);
      }
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_STAR_EQUAL);
      return makeToken(lexer, TOKEN_STAR);
    case '/':
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_SLASH_EQUAL);
      return makeToken(lexer, TOKEN_SLASH);
    case '%':
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_PERCENT_EQUAL);
      return makeToken(lexer, TOKEN_PERCENT);

    case '!':
      if (match(lexer, '=')) {
        return makeToken(lexer, match(lexer, '=') ? TOKEN_BANG_EQUAL_EQUAL
                                                  : TOKEN_BANG_EQUAL);
      }
      return makeToken(lexer, TOKEN_BANG);

    case '=':
      if (match(lexer, '>')) return makeToken(lexer, TOKEN_ARROW);
      if (match(lexer, '=')) {
        return makeToken(lexer, match(lexer, '=') ? TOKEN_EQUAL_EQUAL_EQUAL
                                                  : TOKEN_EQUAL_EQUAL);
      }
      return makeToken(lexer, TOKEN_EQUAL);

    case '<': return makeToken(lexer, match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>': return makeToken(lexer, match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

    case '&':
      if (match(lexer, '&')) {
        if (match(lexer, '=')) return makeToken(lexer, TOKEN_AMP_AMP_EQUAL);
        return makeToken(lexer, TOKEN_AMP_AMP);
      }
      return errorToken(lexer, "unexpected '&' (did you mean '&&'?)");

    case '|':
      if (match(lexer, '|')) {
        if (match(lexer, '=')) return makeToken(lexer, TOKEN_PIPE_PIPE_EQUAL);
        return makeToken(lexer, TOKEN_PIPE_PIPE);
      }
      return errorToken(lexer, "unexpected '|' (did you mean '||'?)");

    case '"':
    case '\'':
      return string(lexer, c);

    case '`':
      return templateLiteral(lexer);
  }

  return errorToken(lexer, "unexpected character");
}
