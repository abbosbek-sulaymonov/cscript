/* diagnostic.h — source-located errors shared by the lexer, parser and compiler.
 *
 * Every stage reports through this so messages look the same and the caller can
 * ask "did anything fail?" once, instead of threading status codes around.
 */
#ifndef CSCRIPT_DIAGNOSTIC_H
#define CSCRIPT_DIAGNOSTIC_H

#include "cscript/common.h"

typedef struct {
  const char *source;     /* whole source text, for quoting the offending line */
  const char *sourceName; /* file name, or "<repl>" */
  int errorCount;
  bool panicMode; /* set while recovering, to suppress cascading errors */

  /* Counts errors without printing them.
   *
   * For asking a question of the parser rather than compiling: the REPL parses
   * each line twice before it runs it — once as typed, once with a semicolon
   * appended — to find out which of the two the user meant. The failed attempt
   * is not an error the user made and must not be reported as one. */
  bool quiet;
} Diagnostics;

void csDiagnosticsInit(Diagnostics *diag, const char *source, const char *sourceName);

/* Reports at a source span. `at`/`length` point into `source`. Suppressed while
 * panic mode is set, so one syntax error does not produce ten messages. */
void csDiagnosticError(Diagnostics *diag, int line, const char *at, int length, const char *format, ...);

static inline bool csDiagnosticsFailed(const Diagnostics *diag) {
  return diag->errorCount > 0;
}

#endif /* CSCRIPT_DIAGNOSTIC_H */
