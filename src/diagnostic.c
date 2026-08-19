#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/diagnostic.h"

void csDiagnosticsInit(Diagnostics *diag, const char *source,
                       const char *sourceName) {
  diag->source = source;
  diag->sourceName = sourceName;
  diag->errorCount = 0;
  diag->panicMode = false;
}

/* Finds the start of the line containing `at`, walking back from it. */
static const char *lineStart(const char *source, const char *at) {
  const char *start = at;
  while (start > source && start[-1] != '\n') start--;
  return start;
}

static int lineLength(const char *start) {
  const char *end = start;
  while (*end != '\0' && *end != '\n') end++;
  return (int)(end - start);
}

void csDiagnosticError(Diagnostics *diag, int line, const char *at, int length,
                       const char *format, ...) {
  /* One syntax error usually knocks the parser off the rails; stay quiet until
   * it resynchronises so the user sees the real problem, not the aftershocks. */
  if (diag->panicMode) return;
  diag->panicMode = true;
  diag->errorCount++;

  fprintf(stderr, "%s:%d: error: ", diag->sourceName, line);

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  if (diag->source == NULL || at == NULL) return;
  if (at < diag->source) return;

  const char *start = lineStart(diag->source, at);
  int width = lineLength(start);
  fprintf(stderr, "  %5d | %.*s\n", line, width, start);

  /* Underline the offending span beneath the quoted line. */
  int column = (int)(at - start);
  fprintf(stderr, "        | ");
  for (int i = 0; i < column; i++) {
    fputc(start[i] == '\t' ? '\t' : ' ', stderr);
  }
  int caretCount = length > 0 ? length : 1;
  for (int i = 0; i < caretCount; i++) fputc('^', stderr);
  fprintf(stderr, "\n");
}
