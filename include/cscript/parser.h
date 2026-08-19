/* parser.h — recursive descent with Pratt-style binary precedence.
 *
 * Produces an AST in the supplied arena. On a syntax error it reports through
 * the diagnostics object and synchronises to the next statement boundary, so
 * one bad line does not bury the rest of the file.
 */
#ifndef CSCRIPT_PARSER_H
#define CSCRIPT_PARSER_H

#include "cscript/ast.h"
#include "cscript/common.h"
#include "cscript/diagnostic.h"
#include "cscript/lexer.h"

typedef struct {
  Lexer lexer;
  Token current;
  Token previous;
  AstArena *arena;
  Diagnostics *diag;
} Parser;

/* Returns the program node, or NULL if the source had a syntax error. */
AstNode *csParse(const char *source, AstArena *arena, Diagnostics *diag);

#endif /* CSCRIPT_PARSER_H */
