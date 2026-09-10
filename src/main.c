/* main.c — the command line, and the REPL behind it.
 *
 * Options are parsed before the VM starts, because two of them decide how it
 * starts: the tiering threshold has to be set before the first back-edge
 * counts, and the stages to print have to be set before the first source is
 * read. So this file does its argument handling first and touches nothing else
 * until it knows what it was asked for.
 *
 * Exit codes follow sysexits, which is what a shell script checking one
 * expects: 64 for a usage mistake, 65 for a program that would not compile, 66
 * for a file that could not be read, 70 for one that failed while running.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/common.h"
#include "cscript/debug.h"
#include "cscript/jit.h"
#include "cscript/lexer.h"
#include "cscript/parser.h"
#include "cscript/module.h"
#include "cscript/vm.h"

#ifdef CS_DEBUG_PROFILE_OPCODES
void csVMDumpOpcodeProfile(void);
#endif

#define EXIT_OK 0
#define EXIT_USAGE 64
#define EXIT_COMPILE 65
#define EXIT_NO_INPUT 66
#define EXIT_RUNTIME 70

/* One REPL entry may span several lines, and a program piped through stdin has
 * no line structure at all, so both grow a buffer rather than assuming one. */
#define READ_CHUNK 4096

typedef struct {
  const char *eval;   /* -e, or NULL */
  const char *script; /* a path, "-" for stdin, or NULL */
  const char *const *args;
  int argCount;
  bool check;
  bool time;
} Options;

static void printUsage(FILE *out) {
  fprintf(out,
          "cscript %s — JavaScript syntax, without the footguns\n"
          "\n"
          "usage:\n"
          "  cscript                     start the REPL\n"
          "  cscript [options] <file.cx> [args...]\n"
          "  cscript [options] -e <code>\n"
          "  cscript [options] -         read the program from stdin\n"
          "\n"
          "options:\n"
          "  -e, --eval <code>     run <code> as a program\n"
          "  -c, --check           compile and type-check, but do not run\n"
          "      --print-tokens    dump the token stream of every file read\n"
          "      --print-ast       dump the parse tree\n"
          "      --print-bytecode  disassemble each compiled chunk\n"
          "      --time            report how long the program took, on stderr\n"
          "      --jit-threshold <n>  how much work before a function compiles\n"
          "      --no-jit          never tier up, whatever gets hot\n"
          "      --jit-report      print what the compiler took, on exit\n"
          "  -h, --help            show this message\n"
          "  -v, --version         print the version\n"
          "  --                    end of options; the next word is the script\n"
          "\n"
          "Arguments after the script are in `process.argv`, as in Node.\n"
          "Exit codes: 0 ran, 64 usage, 65 would not compile, 66 no such file,\n"
          "70 failed while running.\n",
          CS_VERSION_STRING);
}

/* ---- reading whole streams --------------------------------------------- */

/* Reads a stream to its end. Returns a malloc'd string the caller frees, or
 * NULL when the allocation failed. */
static char *readStream(FILE *in) {
  size_t capacity = READ_CHUNK;
  size_t length = 0;
  char *buffer = (char *)malloc(capacity);
  if (buffer == NULL) return NULL;

  for (;;) {
    if (length + READ_CHUNK + 1 > capacity) {
      capacity *= 2;
      char *grown = (char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(buffer);
        return NULL;
      }
      buffer = grown;
    }
    size_t read = fread(buffer + length, 1, READ_CHUNK, in);
    length += read;
    if (read < READ_CHUNK) break;
  }
  buffer[length] = '\0';
  return buffer;
}

/* ---- the REPL ----------------------------------------------------------- */

/* The offset of the entry's final semicolon, or -1 when it has none.
 *
 * Wanted because printing an expression's value is done by wrapping the entry
 * in a `console.log(...)`, and `console.log(1 + 1;)` is not a program. Asked
 * of the lexer rather than by looking at the last character, because the last
 * character may belong to a comment — `1 + 1; // note` ends in a `e`. */
static int trailingSemicolon(const char *source) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, "<repl>");
  diag.quiet = true;

  Lexer lexer;
  csLexerInit(&lexer, source, &diag);

  int at = -1;
  for (;;) {
    Token token = csLexerNext(&lexer);
    if (token.type == TOKEN_EOF || token.type == TOKEN_ERROR) break;
    at = token.type == TOKEN_SEMICOLON ? (int)(token.start - source) : -1;
  }
  return at;
}

/* Whether an entry is still open — a brace, bracket or parenthesis that has
 * not been closed — so the REPL knows to keep reading rather than report a
 * syntax error the user was about to fix.
 *
 * Counted with the real lexer rather than by scanning characters, because a
 * brace inside a string or a comment closes nothing and a template literal
 * contains both. */
static bool entryIsOpen(const char *source) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, "<repl>");
  diag.quiet = true;

  Lexer lexer;
  csLexerInit(&lexer, source, &diag);

  int depth = 0;
  for (;;) {
    Token token = csLexerNext(&lexer);
    if (token.type == TOKEN_EOF) break;
    /* An unterminated string or template is its own kind of open: the lexer
     * reports it and stops, and one more line may well close it. */
    if (token.type == TOKEN_ERROR) return true;
    switch (token.type) {
      case TOKEN_LEFT_BRACE:
      case TOKEN_LEFT_BRACKET:
      case TOKEN_LEFT_PAREN:
        depth++;
        break;
      case TOKEN_RIGHT_BRACE:
      case TOKEN_RIGHT_BRACKET:
      case TOKEN_RIGHT_PAREN:
        depth--;
        break;
      default:
        break;
    }
  }
  return depth > 0;
}

/* Does this parse, and is it one expression?
 *
 * Asked of the parser rather than guessed at from the first word, because the
 * answer decides what the REPL does with the entry and a guess would be wrong
 * on exactly the entries people type. `expression` is set when the whole entry
 * is a single expression statement, which is the case whose value is worth
 * printing. */
static bool entryParses(const char *source, bool *expression) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, "<repl>");
  diag.quiet = true;

  AstArena arena;
  csAstArenaInit(&arena);
  AstNode *program = csParse(source, &arena, &diag);

  bool ok = program != NULL && !csDiagnosticsFailed(&diag);
  *expression = ok && program->as.program.count == 1 &&
                program->as.program.statements[0]->type == AST_EXPRESSION_STMT;

  csAstArenaFree(&arena);
  return ok;
}

static void printReplHelp(void) {
  printf(
      "  .help    show this message\n"
      "  .exit    leave the REPL (so does Ctrl-D)\n"
      "\n"
      "An entry that is one expression has its value printed. CScript has no\n"
      "automatic semicolon insertion, so a file needs its semicolons — but an\n"
      "entry here is a fragment rather than a file, and one is added when the\n"
      "entry only needs that to parse. An unclosed brace, bracket or\n"
      "parenthesis keeps the entry open, and `...` asks for the rest of it.\n");
}

static void repl(void) {
  printf("cscript %s — .help for help, Ctrl-D to exit\n", CS_VERSION_STRING);

  /* The entry being built up, across as many lines as it takes. */
  size_t capacity = READ_CHUNK;
  char *entry = (char *)malloc(capacity);
  if (entry == NULL) return;
  size_t length = 0;

  char line[READ_CHUNK];
  for (;;) {
    printf("%s", length == 0 ? "> " : "... ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    size_t lineLength = strlen(line);
    if (length + lineLength + 1 > capacity) {
      capacity = (length + lineLength + 1) * 2;
      char *grown = (char *)realloc(entry, capacity);
      if (grown == NULL) break;
      entry = grown;
    }
    memcpy(entry + length, line, lineLength + 1);
    length += lineLength;

    /* Nothing typed yet: no entry to run and no error to report. */
    const char *cursor = entry;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
      cursor++;
    }
    if (*cursor == '\0') {
      length = 0;
      continue;
    }

    if (length == lineLength) {
      if (strcmp(cursor, ".exit\n") == 0 || strcmp(cursor, ".exit") == 0) break;
      if (strcmp(cursor, ".help\n") == 0 || strcmp(cursor, ".help") == 0) {
        printReplHelp();
        length = 0;
        continue;
      }
    }

    if (entryIsOpen(entry)) continue;

    /* What the entry is decides what to do with it. Three attempts, in the
     * order that gives the user what they meant: as typed, as typed with the
     * semicolon the language wants, and — when neither parses — as typed
     * again, this time loudly, so the error they see is about what they
     * actually wrote. */
    bool expression = false;
    char *withSemicolon = NULL;
    const char *toRun = entry;

    if (!entryParses(entry, &expression)) {
      size_t size = length + 2;
      withSemicolon = (char *)malloc(size);
      if (withSemicolon != NULL) {
        memcpy(withSemicolon, entry, length);
        withSemicolon[length] = ';';
        withSemicolon[length + 1] = '\0';
        if (entryParses(withSemicolon, &expression)) {
          toRun = withSemicolon;
        } else {
          free(withSemicolon);
          withSemicolon = NULL;
          expression = false;
        }
      }
    }

    if (expression) {
      /* Printed by wrapping rather than by teaching the compiler to leave a
       * value behind, because the wrap is exactly as correct and costs the
       * rest of the pipeline nothing.
       *
       * Two details make it hold up. The newlines, because a trailing `//`
       * comment would otherwise swallow the closing bracket; and blanking the
       * entry's own semicolon, because the statement's terminator has no
       * meaning once the statement is an argument. */
      size_t size = length + 32;
      char *wrapped = (char *)malloc(size);
      if (wrapped != NULL) {
        char *inner = (char *)malloc(length + 1);
        if (inner != NULL) {
          memcpy(inner, entry, length + 1);
          int semicolon = trailingSemicolon(inner);
          if (semicolon >= 0) inner[semicolon] = ' ';
          snprintf(wrapped, size, "console.log(\n%s\n);", inner);
          csInterpret(wrapped, "<repl>");
          free(inner);
        }
        free(wrapped);
      }
    } else {
      csInterpret(toRun, "<repl>");
    }

    free(withSemicolon);
    length = 0;
  }

  free(entry);
}

/* ---- running ------------------------------------------------------------ */

static int codeFor(InterpretResult result) {
  if (result == CS_COMPILE_ERROR) return EXIT_COMPILE;
  if (result == CS_RUNTIME_ERROR) return EXIT_RUNTIME;
  return EXIT_OK;
}

/* ---- options ------------------------------------------------------------ */

/* Reads the option that takes a value, which may be attached with `=` or be
 * the next word. Returns NULL when it is missing, having said so. */
static const char *optionValue(const char *option, const char *inlineValue,
                               int argc, const char *argv[], int *index) {
  if (inlineValue != NULL) return inlineValue;
  if (*index + 1 < argc) return argv[++(*index)];
  fprintf(stderr, "cscript: %s needs a value\n", option);
  return NULL;
}

/* Fills `options` from the command line. Returns an exit code, or -1 to carry
 * on: 0 means the work is already done, which is what --help and --version
 * are. */
static int parseOptions(int argc, const char *argv[], Options *options) {
  memset(options, 0, sizeof *options);

  int i = 1;
  bool endOfOptions = false;
  for (; i < argc; i++) {
    const char *argument = argv[i];

    /* Not an option: the script, and everything after it belongs to it. */
    if (endOfOptions || argument[0] != '-' || strcmp(argument, "-") == 0) {
      options->script = argument;
      options->args = &argv[i + 1];
      options->argCount = argc - i - 1;
      return -1;
    }
    if (strcmp(argument, "--") == 0) {
      endOfOptions = true;
      continue;
    }

    /* `--name=value`, so the value can be attached. */
    const char *equals = strchr(argument, '=');
    char name[64];
    const char *inlineValue = NULL;
    if (equals != NULL && (size_t)(equals - argument) < sizeof name) {
      memcpy(name, argument, (size_t)(equals - argument));
      name[equals - argument] = '\0';
      inlineValue = equals + 1;
      argument = name;
    }

    if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
      printUsage(stdout);
      return EXIT_OK;
    }
    if (strcmp(argument, "-v") == 0 || strcmp(argument, "--version") == 0) {
      printf("cscript %s\n", CS_VERSION_STRING);
      return EXIT_OK;
    }
    if (strcmp(argument, "-e") == 0 || strcmp(argument, "--eval") == 0) {
      const char *code = optionValue("-e", inlineValue, argc, argv, &i);
      if (code == NULL) return EXIT_USAGE;
      options->eval = code;
      /* Everything after the code is the program's, as with a script. */
      options->args = &argv[i + 1];
      options->argCount = argc - i - 1;
      return -1;
    }
    if (strcmp(argument, "-c") == 0 || strcmp(argument, "--check") == 0) {
      options->check = true;
      continue;
    }
    if (strcmp(argument, "--print-tokens") == 0) {
      csDumpStages |= CS_DUMP_TOKENS;
      continue;
    }
    if (strcmp(argument, "--print-ast") == 0) {
      csDumpStages |= CS_DUMP_AST;
      continue;
    }
    if (strcmp(argument, "--print-bytecode") == 0) {
      csDumpStages |= CS_DUMP_BYTECODE;
      continue;
    }
    if (strcmp(argument, "--time") == 0) {
      options->time = true;
      continue;
    }
    if (strcmp(argument, "--jit-threshold") == 0) {
      const char *value = optionValue("--jit-threshold", inlineValue, argc, argv, &i);
      if (value == NULL) return EXIT_USAGE;
      char *end = NULL;
      long threshold = strtol(value, &end, 10);
      if (end == value || *end != '\0' || threshold < 1) {
        fprintf(stderr, "cscript: --jit-threshold wants a positive number, got '%s'\n",
                value);
        return EXIT_USAGE;
      }
      csJitSetThreshold((int)threshold);
      continue;
    }
    if (strcmp(argument, "--no-jit") == 0) {
      csJitDisable();
      continue;
    }
    if (strcmp(argument, "--jit-report") == 0) {
      csJitRequestReport();
      continue;
    }

    fprintf(stderr, "cscript: unknown option '%s'\n\n", argv[i]);
    printUsage(stderr);
    return EXIT_USAGE;
  }

  /* Options only, and none of them said what to run. */
  return -1;
}

int main(int argc, const char *argv[]) {
  Options options;
  int early = parseOptions(argc, argv, &options);
  if (early >= 0) return early;

  csVMInit();

  const char *executable = argc > 0 ? argv[0] : "cscript";
  bool fromStdin = options.script != NULL && strcmp(options.script, "-") == 0;
  csVMSetScriptArgs(executable, fromStdin ? NULL : options.script, options.args,
                    options.argCount);

  struct timespec started;
  if (options.time) clock_gettime(CLOCK_MONOTONIC, &started);

  int exitCode = EXIT_OK;
  char *stdinSource = NULL;

  if (options.eval != NULL) {
    exitCode = codeFor(options.check ? csCheck(options.eval, "<argv>")
                                     : csInterpret(options.eval, "<argv>"));
  } else if (fromStdin) {
    stdinSource = readStream(stdin);
    if (stdinSource == NULL) {
      fprintf(stderr, "cscript: could not read stdin\n");
      exitCode = EXIT_NO_INPUT;
    } else {
      exitCode = codeFor(options.check ? csCheck(stdinSource, "<stdin>")
                                       : csInterpret(stdinSource, "<stdin>"));
    }
  } else if (options.script != NULL) {
    /* Said before the loader gets a chance to blame the compiler for it: a
     * file that is not there is not a program that will not compile. */
    FILE *probe = fopen(options.script, "rb");
    if (probe == NULL) {
      fprintf(stderr, "cscript: could not open '%s'\n", options.script);
      exitCode = EXIT_NO_INPUT;
    } else {
      fclose(probe);
      exitCode = codeFor(options.check ? csCheckFile(options.script)
                                       : csRunFile(options.script));
    }
  } else if (options.check) {
    fprintf(stderr, "cscript: --check needs a script, -e <code>, or -\n");
    exitCode = EXIT_USAGE;
  } else {
    repl();
  }

  if (options.time) {
    struct timespec ended;
    clock_gettime(CLOCK_MONOTONIC, &ended);
    double ms = (double)(ended.tv_sec - started.tv_sec) * 1000.0 +
                (double)(ended.tv_nsec - started.tv_nsec) / 1.0e6;
    fprintf(stderr, "cscript: %.1f ms\n", ms);
  }

#ifdef CS_DEBUG_PROFILE_OPCODES
  csVMDumpOpcodeProfile();
#endif
#ifdef CS_DEBUG_JIT
  csJitDumpProfile();
#endif

  free(stdinSource);
  csVMFree();
  return exitCode;
}
