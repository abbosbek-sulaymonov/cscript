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
} Diagnostics;

void csDiagnosticsInit(Diagnostics *diag, const char *source,
                       const char *sourceName);

/* Reports at a source span. `at`/`length` point into `source`. Suppressed while
 * panic mode is set, so one syntax error does not produce ten messages. */
void csDiagnosticError(Diagnostics *diag, int line, const char *at, int length,
                       const char *format, ...);

static inline bool csDiagnosticsFailed(const Diagnostics *diag) {
  return diag->errorCount > 0;
}

#endif /* CSCRIPT_DIAGNOSTIC_H */
