/* lexer_keyword.c — which identifiers are keywords, and what each token is called.
 *
 * A hand-written trie rather than a hash: the first character narrows it to a
 * handful, and comparing the rest is one memcmp. Every keyword CScript has is
 * spelled out here once — including the ones it has *in order to reject*, like
 * `var`, which has to be a keyword to be refused by name rather than silently
 * parsed as an identifier.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/lexer.h"

#include "compiler/lexer_internal.h"


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
TokenType csLexerIdentifierType(const Lexer *lexer) {
  switch (lexer->start[0]) {
    case 'a': return checkKeyword(lexer, 1, 4, "wait", TOKEN_AWAIT);
    case 'b': return checkKeyword(lexer, 1, 4, "reak", TOKEN_BREAK);
    case 'd':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'e':
            /* default / delete share "de". */
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 'l') {
              return checkKeyword(lexer, 2, 4, "lete", TOKEN_DELETE);
            }
            return checkKeyword(lexer, 2, 5, "fault", TOKEN_DEFAULT);
          case 'o': return checkKeyword(lexer, 2, 0, "", TOKEN_DO);
        }
      }
      break;
    case 'e':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'l': return checkKeyword(lexer, 2, 2, "se", TOKEN_ELSE);
          case 'x':
            /* extends / export share "ex". */
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 'p') {
              return checkKeyword(lexer, 2, 4, "port", TOKEN_EXPORT);
            }
            return checkKeyword(lexer, 2, 5, "tends", TOKEN_EXTENDS);
        }
      }
      break;
    case 'r': return checkKeyword(lexer, 1, 5, "eturn", TOKEN_RETURN);
    case 'w': return checkKeyword(lexer, 1, 4, "hile", TOKEN_WHILE);
    case 'y': return checkKeyword(lexer, 1, 4, "ield", TOKEN_YIELD);
    case 's':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'w': return checkKeyword(lexer, 2, 4, "itch", TOKEN_SWITCH);
          case 'u': return checkKeyword(lexer, 2, 3, "per", TOKEN_SUPER);
          case 't': return checkKeyword(lexer, 2, 4, "atic", TOKEN_STATIC);
        }
      }
      break;
    case 'T': return checkKeyword(lexer, 1, 3, "rue", TOKEN_TRUE);
    case 'l': return checkKeyword(lexer, 1, 2, "et", TOKEN_LET);
    case 'v':
      /* `var` and `void` share their first letter. */
      if (lexer->current - lexer->start > 1 && lexer->start[1] == 'o') {
        return checkKeyword(lexer, 1, 3, "oid", TOKEN_VOID);
      }
      return checkKeyword(lexer, 1, 2, "ar", TOKEN_VAR);
    case 'i':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'f': return checkKeyword(lexer, 2, 0, "", TOKEN_IF);
          case 'm': return checkKeyword(lexer, 2, 4, "port", TOKEN_IMPORT);
          case 'n':
            /* in / instanceof share "in". */
            if (lexer->current - lexer->start == 2) return TOKEN_IN;
            return checkKeyword(lexer, 2, 8, "stanceof", TOKEN_INSTANCEOF);
        }
      }
      break;
    case 'n':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'u': return checkKeyword(lexer, 2, 2, "ll", TOKEN_NULL);
          case 'e': return checkKeyword(lexer, 2, 1, "w", TOKEN_NEW);
        }
      }
      break;
    case 'u': return checkKeyword(lexer, 1, 8, "ndefined", TOKEN_UNDEFINED);
    case 'F': return checkKeyword(lexer, 1, 7, "unction", TOKEN_FUNCTION);
    case 'c':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'a':
            /* case / catch both start "ca". */
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 't') {
              return checkKeyword(lexer, 1, 4, "atch", TOKEN_CATCH);
            }
            return checkKeyword(lexer, 1, 3, "ase", TOKEN_CASE);
          case 'l': return checkKeyword(lexer, 2, 3, "ass", TOKEN_CLASS);
          case 'o':
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 'n') {
              /* const / continue share the "con" prefix. */
              if (lexer->current - lexer->start > 3 && lexer->start[3] == 's') {
                return checkKeyword(lexer, 1, 4, "onst", TOKEN_CONST);
              }
              return checkKeyword(lexer, 1, 7, "ontinue", TOKEN_CONTINUE);
            }
            break;
        }
      }
      break;
    case 't':
      if (lexer->current - lexer->start > 1) {
        switch (lexer->start[1]) {
          case 'r':
            /* true / try / throw all start "tr". */
            if (lexer->current - lexer->start > 2) {
              switch (lexer->start[2]) {
                case 'u': return checkKeyword(lexer, 3, 1, "e", TOKEN_TRUE);
                case 'y': return checkKeyword(lexer, 3, 0, "", TOKEN_TRY);
              }
            }
            break;
          case 'h':
            /* this / throw share "th". */
            if (lexer->current - lexer->start > 2 && lexer->start[2] == 'i') {
              return checkKeyword(lexer, 2, 2, "is", TOKEN_THIS);
            }
            return checkKeyword(lexer, 2, 3, "row", TOKEN_THROW);
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
          case 'i': return checkKeyword(lexer, 2, 5, "nally", TOKEN_FINALLY);
        }
      }
      break;
  }
  return TOKEN_IDENTIFIER;
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
    case TOKEN_ELLIPSIS:          return "ELLIPSIS";
    case TOKEN_SEMICOLON:         return "SEMICOLON";
    case TOKEN_COLON:             return "COLON";
    case TOKEN_QUESTION:          return "QUESTION";
    case TOKEN_QUESTION_DOT:      return "QUESTION_DOT";
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
    case TOKEN_STAR_STAR:         return "STAR_STAR";
    case TOKEN_STAR_STAR_EQUAL:   return "STAR_STAR_EQUAL";
    case TOKEN_ARROW:             return "ARROW";
    case TOKEN_OF:                return "OF";
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
    case TOKEN_QUESTION_QUESTION: return "QUESTION_QUESTION";
    case TOKEN_AMP_AMP_EQUAL:     return "AMP_AMP_EQUAL";
    case TOKEN_PIPE_PIPE_EQUAL:   return "PIPE_PIPE_EQUAL";
    case TOKEN_QUESTION_QUESTION_EQUAL: return "QUESTION_QUESTION_EQUAL";
    case TOKEN_IDENTIFIER:        return "IDENTIFIER";
    case TOKEN_PRIVATE_NAME:      return "PRIVATE_NAME";
    case TOKEN_STRING:            return "STRING";
    case TOKEN_NUMBER:            return "NUMBER";
    case TOKEN_BIGINT:            return "BIGINT";
    case TOKEN_TEMPLATE:          return "TEMPLATE";
    case TOKEN_SWITCH:            return "SWITCH";
    case TOKEN_CASE:              return "CASE";
    case TOKEN_DEFAULT:           return "DEFAULT";
    case TOKEN_BREAK:             return "BREAK";
    case TOKEN_CONTINUE:          return "CONTINUE";
    case TOKEN_TRY:               return "TRY";
    case TOKEN_CATCH:             return "CATCH";
    case TOKEN_FINALLY:           return "FINALLY";
    case TOKEN_THROW:             return "THROW";
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
    case TOKEN_AWAIT:             return "AWAIT";
    case TOKEN_DO:                return "DO";
    case TOKEN_DELETE:            return "DELETE";
    case TOKEN_YIELD:             return "YIELD";
    case TOKEN_VOID:              return "VOID";
    case TOKEN_REGEX:             return "REGEX";
    case TOKEN_IN:                return "IN";
    case TOKEN_IMPORT:            return "IMPORT";
    case TOKEN_EXPORT:            return "EXPORT";
    case TOKEN_CLASS:             return "CLASS";
    case TOKEN_EXTENDS:           return "EXTENDS";
    case TOKEN_NEW:               return "NEW";
    case TOKEN_THIS:              return "THIS";
    case TOKEN_SUPER:             return "SUPER";
    case TOKEN_STATIC:            return "STATIC";
    case TOKEN_INSTANCEOF:        return "INSTANCEOF";
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
