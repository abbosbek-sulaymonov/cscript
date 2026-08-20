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

  /* Set between reading `async` and building the function node it belongs to.
   * `async` is contextual, so it is recognised at the call site and handed to
   * whichever of the three function forms follows it. */
  bool pendingAsync;
  /* Set by whoever read the `*`, consumed by parseFunctionRest — the same
   * hand-off `async` uses, because both modifiers are read before the name
   * and applied after it. */
  bool pendingGenerator;

  /* How many async function bodies enclose the point being parsed, so `await`
   * can be rejected where it means nothing. */
  int asyncDepth;

  /* Whether the body being parsed is a generator's, so `yield` can be
   * rejected where it means nothing. Not a depth: a `yield` belongs to the
   * nearest function body, and an ordinary function nested in a generator is
   * not one. */
  bool inGenerator;
} Parser;

/* Returns the program node, or NULL if the source had a syntax error. */
AstNode *csParse(const char *source, AstArena *arena, Diagnostics *diag);

#endif /* CSCRIPT_PARSER_H */
