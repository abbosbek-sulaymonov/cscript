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

/* Compares the tail of the current lexeme against a keyword suffix. */
static TokenType checkKeyword(const Lexer *lexer, int start, int length,
                              const char *rest, TokenType type) {
  if (lexer->current - lexer->start == start + length &&
      memcmp(lexer->start + start, rest, (size_t)length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

/* Trie over the keyword set: one switch per shared prefix. */
static TokenType identifierType(const Lexer *lexer) {
  switch (lexer->start[0]) {
    case 'b': return checkKeyword(lexer, 1, 4, "reak", TOKEN_BREAK);
    case 'd': return checkKeyword(lexer, 1, 6, "efault", TOKEN_DEFAULT);
    case 'e': return checkKeyword(lexer, 1, 3, "lse", TOKEN_ELSE);
    case 'r': return checkKeyword(lexer, 1, 5, "eturn", TOKEN_RETURN);
    case 'w': return checkKeyword(lexer, 1, 4, "hile", TOKEN_WHILE);
    case 's': return checkKeyword(lexer, 1, 5, "witch", TOKEN_SWITCH);
    case 'l': return checkKeyword(lexer, 1, 2, "et", TOKEN_LET);
    case 'v': return checkKeyword(lexer, 1, 2, "ar", TOKEN_VAR);
    case 'i': return checkKeyword(lexer, 1, 1, "f", TOKEN_IF);
    case 'n': return checkKeyword(lexer, 1, 3, "ull", TOKEN_NULL);
    case 'u': return checkKeyword(lexer, 1, 8, "ndefined", TOKEN_UNDEFINED);
    case 'F': return checkKeyword(lexer, 1, 7, "unction", TOKEN_FUNCTION);
    case 'c':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'o':
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 'n') {
              /* const / continue share the "con" prefix. */
              if (lexer->current - lexer->start > 3 && lexer->start[3] == 's') {
                return checkKeyword(lexer, 1, 4, "onst", TOKEN_CONST);
              }
              return checkKeyword(lexer, 1, 7, "ontinue", TOKEN_CONTINUE);
            }
            break;
          case 'a': return checkKeyword(lexer, 1, 3, "ase", TOKEN_CASE);
        }
      }
      break;
    case 't':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'r': return checkKeyword(lexer, 2, 2, "ue", TOKEN_TRUE);
          case 'y': return checkKeyword(lexer, 2, 4, "peof", TOKEN_TYPEOF);
        }
      }
      break;
    case 'f':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'a': return checkKeyword(lexer, 2, 3, "lse", TOKEN_FALSE);
          case 'o': return checkKeyword(lexer, 2, 1, "r", TOKEN_FOR);
          case 'u': return checkKeyword(lexer, 2, 6, "nction", TOKEN_FUNCTION);
        }
      }
      break;
  }
  return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer *lexer) {
  while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
  return makeToken(lexer, identifierType(lexer));
}

static Token number(Lexer *lexer) {
  /* Hex literals: 0x1F. */
  if (lexer->current[-1] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
    advance(lexer);
    if (!isHexDigit(peek(lexer))) return errorToken(lexer, "expected hex digits after '0x'");
    while (isHexDigit(peek(lexer))) advance(lexer);
    return makeToken(lexer, TOKEN_NUMBER);
  }

  while (isDigit(peek(lexer))) advance(lexer);

  if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
    advance(lexer);
    while (isDigit(peek(lexer))) advance(lexer);
  }

  /* Exponent: 1e10, 2.5E-3. */
  if (peek(lexer) == 'e' || peek(lexer) == 'E') {
    const char *rewind = lexer->current;
    advance(lexer);
    if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
    if (isDigit(peek(lexer))) {
      while (isDigit(peek(lexer))) advance(lexer);
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

Token csLexerNext(Lexer *lexer) {
  skipWhitespaceAndComments(lexer);
  lexer->start = lexer->current;

  if (isAtEnd(lexer)) return makeToken(lexer, TOKEN_EOF);

  char c = advance(lexer);

  if (isAlpha(c)) return identifier(lexer);
  if (isDigit(c)) return number(lexer);

  switch (c) {
    case '(': return makeToken(lexer, TOKEN_LEFT_PAREN);
    case ')': return makeToken(lexer, TOKEN_RIGHT_PAREN);
    case '{': return makeToken(lexer, TOKEN_LEFT_BRACE);
    case '}': return makeToken(lexer, TOKEN_RIGHT_BRACE);
    case '[': return makeToken(lexer, TOKEN_LEFT_BRACKET);
    case ']': return makeToken(lexer, TOKEN_RIGHT_BRACKET);
    case ',': return makeToken(lexer, TOKEN_COMMA);
    case '.': return makeToken(lexer, TOKEN_DOT);
    case ';': return makeToken(lexer, TOKEN_SEMICOLON);
    case ':': return makeToken(lexer, TOKEN_COLON);
    case '?': return makeToken(lexer, TOKEN_QUESTION);
    case '+':
      if (match(lexer, '+')) return makeToken(lexer, TOKEN_PLUS_PLUS);
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_PLUS_EQUAL);
      return makeToken(lexer, TOKEN_PLUS);
    case '-':
      if (match(lexer, '-')) return makeToken(lexer, TOKEN_MINUS_MINUS);
      if (match(lexer, '=')) return makeToken(lexer, TOKEN_MINUS_EQUAL);
      return makeToken(lexer, TOKEN_MINUS);
    case '*':
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
      if (match(lexer, '=')) {
        return makeToken(lexer, match(lexer, '=') ? TOKEN_EQUAL_EQUAL_EQUAL
                                                  : TOKEN_EQUAL_EQUAL);
      }
      return makeToken(lexer, TOKEN_EQUAL);

    case '<': return makeToken(lexer, match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>': return makeToken(lexer, match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

    case '&':
      if (match(lexer, '&')) return makeToken(lexer, TOKEN_AMP_AMP);
      return errorToken(lexer, "unexpected '&' (did you mean '&&'?)");

    case '|':
      if (match(lexer, '|')) return makeToken(lexer, TOKEN_PIPE_PIPE);
      return errorToken(lexer, "unexpected '|' (did you mean '||'?)");

    case '"':
    case '\'':
      return string(lexer, c);

    case '`':
      return templateLiteral(lexer);
  }

  return errorToken(lexer, "unexpected character");
}

const char *csTokenTypeName(TokenType type) {
  switch (type) {
    case TOKEN_LEFT_PAREN:        return "LEFT_PAREN";
    case TOKEN_RIGHT_PAREN:       return "RIGHT_PAREN";
    case TOKEN_LEFT_BRACE:        return "LEFT_BRACE";
    case TOKEN_RIGHT_BRACE:       return "RIGHT_BRACE";
    case TOKEN_LEFT_BRACKET:      return "LEFT_BRACKET";
    case TOKEN_RIGHT_BRACKET:     return "RIGHT_BRACKET";
    case TOKEN_COMMA:             return "COMMA";
    case TOKEN_DOT:               return "DOT";
    case TOKEN_SEMICOLON:         return "SEMICOLON";
    case TOKEN_COLON:             return "COLON";
    case TOKEN_QUESTION:          return "QUESTION";
    case TOKEN_PLUS:              return "PLUS";
    case TOKEN_MINUS:             return "MINUS";
    case TOKEN_STAR:              return "STAR";
    case TOKEN_SLASH:             return "SLASH";
    case TOKEN_PERCENT:           return "PERCENT";
    case TOKEN_PLUS_PLUS:         return "PLUS_PLUS";
    case TOKEN_MINUS_MINUS:       return "MINUS_MINUS";
    case TOKEN_PLUS_EQUAL:        return "PLUS_EQUAL";
    case TOKEN_MINUS_EQUAL:       return "MINUS_EQUAL";
    case TOKEN_STAR_EQUAL:        return "STAR_EQUAL";
    case TOKEN_SLASH_EQUAL:       return "SLASH_EQUAL";
    case TOKEN_PERCENT_EQUAL:     return "PERCENT_EQUAL";
    case TOKEN_BANG:              return "BANG";
    case TOKEN_BANG_EQUAL:        return "BANG_EQUAL";
    case TOKEN_BANG_EQUAL_EQUAL:  return "BANG_EQUAL_EQUAL";
    case TOKEN_EQUAL:             return "EQUAL";
    case TOKEN_EQUAL_EQUAL:       return "EQUAL_EQUAL";
    case TOKEN_EQUAL_EQUAL_EQUAL: return "EQUAL_EQUAL_EQUAL";
    case TOKEN_GREATER:           return "GREATER";
    case TOKEN_GREATER_EQUAL:     return "GREATER_EQUAL";
    case TOKEN_LESS:              return "LESS";
    case TOKEN_LESS_EQUAL:        return "LESS_EQUAL";
    case TOKEN_AMP_AMP:           return "AMP_AMP";
    case TOKEN_PIPE_PIPE:         return "PIPE_PIPE";
    case TOKEN_IDENTIFIER:        return "IDENTIFIER";
    case TOKEN_STRING:            return "STRING";
    case TOKEN_NUMBER:            return "NUMBER";
    case TOKEN_TEMPLATE:          return "TEMPLATE";
    case TOKEN_SWITCH:            return "SWITCH";
    case TOKEN_CASE:              return "CASE";
    case TOKEN_DEFAULT:           return "DEFAULT";
    case TOKEN_BREAK:             return "BREAK";
    case TOKEN_CONTINUE:          return "CONTINUE";
    case TOKEN_TRUE:              return "TRUE";
    case TOKEN_FALSE:             return "FALSE";
    case TOKEN_NULL:              return "NULL";
    case TOKEN_UNDEFINED:         return "UNDEFINED";
    case TOKEN_LET:               return "LET";
    case TOKEN_CONST:             return "CONST";
    case TOKEN_VAR:               return "VAR";
    case TOKEN_FUNCTION:          return "FUNCTION";
    case TOKEN_RETURN:            return "RETURN";
    case TOKEN_IF:                return "IF";
    case TOKEN_ELSE:              return "ELSE";
    case TOKEN_WHILE:             return "WHILE";
    case TOKEN_FOR:               return "FOR";
    case TOKEN_TYPEOF:            return "TYPEOF";
    case TOKEN_ERROR:             return "ERROR";
    case TOKEN_EOF:               return "EOF";
  }
  return "UNKNOWN";
}

void csLexerDumpTokens(const char *source, Diagnostics *diag) {
  Lexer lexer;
  csLexerInit(&lexer, source, diag);

  printf("== tokens ==\n");
  int line = -1;
  for (;;) {
    Token token = csLexerNext(&lexer);
    if (token.line != line) {
      printf("%4d ", token.line);
      line = token.line;
    } else {
      printf("   | ");
    }
    printf("%-18s '%.*s'\n", csTokenTypeName(token.type), token.length, token.start);
    if (token.type == TOKEN_EOF) break;
  }
  printf("\n");
}
