#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/common.h"
#include "cscript/module.h"
#include "cscript/vm.h"

#ifdef CS_DEBUG_PROFILE_OPCODES
void csVMDumpOpcodeProfile(void);
#endif

#define REPL_LINE_MAX 1024

static void printUsage(FILE *out) {
  fprintf(out,
          "cscript %s — JavaScript syntax, without the footguns\n"
          "\n"
          "usage:\n"
          "  cscript              start the REPL\n"
          "  cscript <file.cx>    run a script\n"
          "  cscript -e <code>    run a one-liner\n"
          "  cscript --version    print the version\n"
          "  cscript --help       show this message\n",
          CS_VERSION_STRING);
}

static void repl(void) {
  printf("cscript %s — Ctrl-D to exit\n", CS_VERSION_STRING);
  char line[REPL_LINE_MAX];

  for (;;) {
    printf("> ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }
    /* Skip blank lines rather than reporting "expected an expression". */
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') cursor++;
    if (*cursor == '\0') continue;

    csInterpret(line, "<repl>");
  }
}

static int runSource(const char *source, const char *name) {
  InterpretResult result = csInterpret(source, name);
  if (result == CS_COMPILE_ERROR) return 65;
  if (result == CS_RUNTIME_ERROR) return 70;
  return 0;
}

int main(int argc, const char *argv[]) {
  csVMInit();
  int exitCode = 0;

  if (argc == 1) {
    repl();
  } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    printUsage(stdout);
  } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
    printf("cscript %s\n", CS_VERSION_STRING);
  } else if (strcmp(argv[1], "-e") == 0) {
    if (argc < 3) {
      fprintf(stderr, "cscript: -e needs a code argument\n");
      exitCode = 64;
    } else {
      exitCode = runSource(argv[2], "<argv>");
    }
  } else if (argv[1][0] == '-') {
    fprintf(stderr, "cscript: unknown option '%s'\n\n", argv[1]);
    printUsage(stderr);
    exitCode = 64;
  } else {
    InterpretResult result = csRunFile(argv[1]);
    exitCode = result == CS_COMPILE_ERROR ? 65 : result == CS_RUNTIME_ERROR ? 70 : 0;
  }

#ifdef CS_DEBUG_PROFILE_OPCODES
  csVMDumpOpcodeProfile();
#endif

  csVMFree();
  return exitCode;
}
