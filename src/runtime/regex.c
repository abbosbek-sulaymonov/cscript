/* regex.c — compiling a pattern to the program the matcher runs.
 *
 * A recursive-descent parser emitting a flat instruction array, with the
 * alternation and repetition operators becoming jumps into it. Written and
 * tested before the language could reach it — tests/regex_engine_test.c links
 * this file alone — which is why the interface is a compiled program and a
 * matcher over it rather than anything to do with Values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/regex.h"

#include "runtime/regex_internal.h"

/* ---- building the program --------------------------------------------- */

static int emit(Regex *regex, ReOp op, int x, int y) {
  if (regex->capacity < regex->count + 1) {
    regex->capacity = regex->capacity < 16 ? 16 : regex->capacity * 2;
    regex->code = (ReInst *)realloc(regex->code, sizeof(ReInst) * (size_t)regex->capacity);
  }
  regex->code[regex->count].op = op;
  regex->code[regex->count].x = x;
  regex->code[regex->count].y = y;
  return regex->count++;
}

static int addClass(Regex *regex, const ReClass *set) {
  if (regex->classCapacity < regex->classCount + 1) {
    regex->classCapacity = regex->classCapacity < 8 ? 8 : regex->classCapacity * 2;
    regex->classes =
        (ReClass *)realloc(regex->classes, sizeof(ReClass) * (size_t)regex->classCapacity);
  }
  regex->classes[regex->classCount] = *set;
  return regex->classCount++;
}

static void classAdd(ReClass *set, unsigned char byte) {
  set->bits[byte >> 3] |= (uint8_t)(1u << (byte & 7));
}

bool csRegexClassHas(const ReClass *set, unsigned char byte) {
  return (set->bits[byte >> 3] & (1u << (byte & 7))) != 0;
}

static void classAddRange(ReClass *set, unsigned char from, unsigned char to) {
  for (int c = from; c <= (int)to; c++) classAdd(set, (unsigned char)c);
}

bool csRegexIsWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_';
}

/* ---- the parser -------------------------------------------------------- */

typedef struct {
  Regex *regex;
  const char *pattern;
  int length;
  int position;
  char *error;
  size_t errorSize;
  bool failed;
} ReParser;

static void fail(ReParser *parser, const char *message) {
  if (!parser->failed) {
    snprintf(parser->error, parser->errorSize, "%s", message);
    parser->failed = true;
  }
}

static bool atEnd(const ReParser *parser) { return parser->position >= parser->length; }
static char peekChar(const ReParser *parser) {
  return atEnd(parser) ? '\0' : parser->pattern[parser->position];
}
static char nextChar(ReParser *parser) { return parser->pattern[parser->position++]; }

static void parseAlternation(ReParser *parser);

/* `\d`, `\w`, `\s` and their negations, as class members rather than as
 * standalone atoms, so `[\d.]` works the same way `\d` alone does. */
static bool addEscapeClass(ReClass *set, char escape) {
  switch (escape) {
    case 'd': classAddRange(set, '0', '9'); return true;
    case 'w':
      classAddRange(set, 'a', 'z');
      classAddRange(set, 'A', 'Z');
      classAddRange(set, '0', '9');
      classAdd(set, '_');
      return true;
    case 's':
      classAdd(set, ' ');
      classAdd(set, '\t');
      classAdd(set, '\n');
      classAdd(set, '\r');
      classAdd(set, '\f');
      classAdd(set, '\v');
      return true;
    default: return false;
  }
}

static void negate(ReClass *set) {
  for (int i = 0; i < 32; i++) set->bits[i] = (uint8_t)~set->bits[i];
}

/* The byte an escape stands for, for the escapes that mean one byte. */
static bool escapedByte(char escape, unsigned char *out) {
  switch (escape) {
    case 'n': *out = '\n'; return true;
    case 't': *out = '\t'; return true;
    case 'r': *out = '\r'; return true;
    case 'f': *out = '\f'; return true;
    case 'v': *out = '\v'; return true;
    case '0': *out = '\0'; return true;
    default:
      /* Anything else escaped is itself: `\.` `\\` `\/` `\(` and so on. */
      *out = (unsigned char)escape;
      return true;
  }
}

/* One byte, matched case-insensitively when the flag is set — which is done by
 * emitting a two-element class rather than by lowering at match time, so the
 * matcher stays one comparison. */
static void emitByte(ReParser *parser, unsigned char byte) {
  Regex *regex = parser->regex;
  if (!regex->ignoreCase) {
    emit(regex, RE_CHAR, byte, 0);
    return;
  }
  ReClass set;
  memset(&set, 0, sizeof set);
  classAdd(&set, byte);
  if (byte >= 'a' && byte <= 'z') classAdd(&set, (unsigned char)(byte - 32));
  if (byte >= 'A' && byte <= 'Z') classAdd(&set, (unsigned char)(byte + 32));
  emit(regex, RE_CLASS, addClass(regex, &set), 0);
}

static void parseClass(ReParser *parser) {
  ReClass set;
  memset(&set, 0, sizeof set);

  bool negated = false;
  if (peekChar(parser) == '^') {
    negated = true;
    parser->position++;
  }

  bool first = true;
  while (!atEnd(parser) && (peekChar(parser) != ']' || first)) {
    first = false;
    unsigned char low;

    if (peekChar(parser) == '\\') {
      parser->position++;
      if (atEnd(parser)) { fail(parser, "trailing '\\' in a character class"); return; }
      char escape = nextChar(parser);
      if (addEscapeClass(&set, escape)) continue;
      if (escape == 'D' || escape == 'W' || escape == 'S') {
        /* A negated shorthand inside a class cannot be folded into the same
         * set without changing what the rest of it means. */
        fail(parser, "\\D, \\W and \\S are not supported inside a character class");
        return;
      }
      escapedByte(escape, &low);
    } else {
      low = (unsigned char)nextChar(parser);
    }

    /* `a-z`, but a `-` at the end of a class is a literal. */
    if (peekChar(parser) == '-' && parser->position + 1 < parser->length &&
        parser->pattern[parser->position + 1] != ']') {
      parser->position++;
      unsigned char high;
      if (peekChar(parser) == '\\') {
        parser->position++;
        if (atEnd(parser)) { fail(parser, "trailing '\\' in a character class"); return; }
        escapedByte(nextChar(parser), &high);
      } else {
        high = (unsigned char)nextChar(parser);
      }
      if (high < low) { fail(parser, "a character range runs backwards"); return; }
      classAddRange(&set, low, high);
      continue;
    }

    classAdd(&set, low);
  }

  if (atEnd(parser)) { fail(parser, "unterminated character class: missing ']'"); return; }
  parser->position++; /* the ']' */

  if (parser->regex->ignoreCase) {
    /* Fold case across the whole set once, rather than at every comparison. */
    for (int c = 'a'; c <= 'z'; c++) {
      if (csRegexClassHas(&set, (unsigned char)c)) classAdd(&set, (unsigned char)(c - 32));
    }
    for (int c = 'A'; c <= 'Z'; c++) {
      if (csRegexClassHas(&set, (unsigned char)c)) classAdd(&set, (unsigned char)(c + 32));
    }
  }
  if (negated) negate(&set);

  emit(parser->regex, RE_CLASS, addClass(parser->regex, &set), 0);
}

/* Parses one atom, returning where its code starts so a quantifier can wrap it. */
/* Records `name` as another way to ask for group `group`. */
static void addGroupName(ReParser *parser, const char *name, int length,
                         int group) {
  Regex *regex = parser->regex;
  for (int i = 0; i < regex->nameCount; i++) {
    if ((int)strlen(regex->names[i].name) == length &&
        memcmp(regex->names[i].name, name, (size_t)length) == 0) {
      fail(parser, "two groups cannot share a name");
      return;
    }
  }

  if (regex->nameCount == regex->nameCapacity) {
    int capacity = regex->nameCapacity < 4 ? 4 : regex->nameCapacity * 2;
    ReGroupName *grown =
        (ReGroupName *)realloc(regex->names, sizeof(ReGroupName) * (size_t)capacity);
    if (grown == NULL) {
      fail(parser, "out of memory recording a group name");
      return;
    }
    regex->names = grown;
    regex->nameCapacity = capacity;
  }

  char *copy = (char *)malloc((size_t)length + 1);
  if (copy == NULL) {
    fail(parser, "out of memory recording a group name");
    return;
  }
  memcpy(copy, name, (size_t)length);
  copy[length] = '\0';
  regex->names[regex->nameCount].name = copy;
  regex->names[regex->nameCount].group = group;
  regex->nameCount++;
}

static int parseAtom(ReParser *parser) {
  Regex *regex = parser->regex;
  int start = regex->count;
  const char *pendingName = NULL;
  int pendingNameLength = 0;
  char c = nextChar(parser);

  switch (c) {
    case '(': {
      bool capturing = true;
      if (parser->position + 1 < parser->length && peekChar(parser) == '?') {
        char kind = parser->pattern[parser->position + 1];
        if (kind == ':') {
          capturing = false;
          parser->position += 2;
        } else if (kind == '=' || kind == '!') {
          parser->position += 2;
          int look = emit(regex, RE_LOOK, kind == '!' ? 1 : 0, 0);

          parseAlternation(parser);
          if (parser->failed) return start;
          if (atEnd(parser) || nextChar(parser) != ')') {
            fail(parser, "unterminated lookahead: missing ')'");
            return start;
          }
          emit(regex, RE_LOOK_END, 0, 0);
          regex->code[look].y = regex->count;
          return start;
        } else if (kind == '<' && parser->position + 2 < parser->length &&
                   (parser->pattern[parser->position + 2] == '=' ||
                    parser->pattern[parser->position + 2] == '!')) {
          bool negated = parser->pattern[parser->position + 2] == '!';
          parser->position += 3;
          int look = emit(regex, RE_LOOKBEHIND, negated ? 1 : 0, 0);

          parseAlternation(parser);
          if (parser->failed) return start;
          if (atEnd(parser) || nextChar(parser) != ')') {
            fail(parser, "unterminated lookbehind: missing ')'");
            return start;
          }
          emit(regex, RE_LOOK_END, 0, 0);
          regex->code[look].y = regex->count;
          return start;
        } else if (kind == '<') {
          /* `(?<name>…)` — a capturing group that also answers to a name. */
          parser->position += 2;
          int nameStart = parser->position;
          while (!atEnd(parser) && peekChar(parser) != '>') parser->position++;
          if (atEnd(parser)) {
            fail(parser, "unterminated group name: missing '>'");
            return start;
          }
          int nameLength = parser->position - nameStart;
          if (nameLength == 0) {
            fail(parser, "a named group needs a name");
            return start;
          }
          parser->position++; /* the '>' */
          pendingName = parser->pattern + nameStart;
          pendingNameLength = nameLength;
        } else {
          fail(parser, "unsupported group modifier after '(?'");
          return start;
        }
      }

      int group = 0;
      if (capturing) {
        if (regex->groupCount >= CS_REGEX_MAX_GROUPS) {
          fail(parser, "too many capture groups");
          return start;
        }
        group = regex->groupCount++;
        emit(regex, RE_SAVE, group * 2, 0);
        if (pendingName != NULL) {
          addGroupName(parser, pendingName, pendingNameLength, group);
          if (parser->failed) return start;
        }
      }

      parseAlternation(parser);
      if (parser->failed) return start;
      if (atEnd(parser) || nextChar(parser) != ')') {
        fail(parser, "unterminated group: missing ')'");
        return start;
      }
      if (capturing) emit(regex, RE_SAVE, group * 2 + 1, 0);
      return start;
    }

    case '[':
      parseClass(parser);
      return start;

    case '.':
      emit(regex, RE_ANY, 0, 0);
      return start;

    case '^':
      emit(regex, RE_ASSERT_BOL, 0, 0);
      return start;

    case '$':
      emit(regex, RE_ASSERT_EOL, 0, 0);
      return start;

    case '\\': {
      if (atEnd(parser)) { fail(parser, "trailing '\\'"); return start; }
      char escape = nextChar(parser);

      if (escape == 'b' || escape == 'B') {
        emit(regex, RE_ASSERT_WORD, escape == 'b' ? 1 : 0, 0);
        return start;
      }
      if (escape >= '1' && escape <= '9') {
        int group = escape - '0';
        if (group >= regex->groupCount) {
          fail(parser, "backreference to a group that does not exist");
          return start;
        }
        emit(regex, RE_BACKREF, group, 0);
        return start;
      }

      ReClass set;
      memset(&set, 0, sizeof set);
      char lower = (char)(escape >= 'A' && escape <= 'Z' ? escape + 32 : escape);
      if (addEscapeClass(&set, lower)) {
        if (escape >= 'A' && escape <= 'Z') negate(&set);
        emit(regex, RE_CLASS, addClass(regex, &set), 0);
        return start;
      }

      unsigned char byte;
      escapedByte(escape, &byte);
      emitByte(parser, byte);
      return start;
    }

    default:
      emitByte(parser, (unsigned char)c);
      return start;
  }
}

static void parseRepetition(ReParser *parser) {
  Regex *regex = parser->regex;
  int atomStart = parseAtom(parser);
  if (parser->failed) return;
  int atomEnd = regex->count;

  char quantifier = peekChar(parser);
  if (quantifier != '*' && quantifier != '+' && quantifier != '?' &&
      quantifier != '{') {
    return;
  }

  int least = 0;
  int most = -1; /* -1 is unbounded */
  if (quantifier == '{') {
    /* `{` is only a quantifier when it parses as one; otherwise it is a
     * literal brace, which is what JavaScript does too. */
    int save = parser->position;
    parser->position++;
    int n = 0;
    bool sawDigit = false;
    while (!atEnd(parser) && peekChar(parser) >= '0' && peekChar(parser) <= '9') {
      n = n * 10 + (nextChar(parser) - '0');
      sawDigit = true;
    }
    if (!sawDigit) { parser->position = save; return; }
    least = n;
    most = n;
    if (peekChar(parser) == ',') {
      parser->position++;
      if (peekChar(parser) == '}') {
        most = -1;
      } else {
        int m = 0;
        while (!atEnd(parser) && peekChar(parser) >= '0' && peekChar(parser) <= '9') {
          m = m * 10 + (nextChar(parser) - '0');
        }
        most = m;
      }
    }
    if (peekChar(parser) != '}') { parser->position = save; return; }
    parser->position++;
    if (most != -1 && most < least) { fail(parser, "a {n,m} range runs backwards"); return; }
    if (most > 1000 || least > 1000) {
      fail(parser, "a {n,m} repetition is limited to 1000");
      return;
    }
  } else {
    parser->position++;
    if (quantifier == '*') { least = 0; most = -1; }
    else if (quantifier == '+') { least = 1; most = -1; }
    else { least = 0; most = 1; }
  }

  /* A trailing `?` makes the quantifier lazy, which swaps the two arms of the
   * split: try the shorter option first. */
  bool lazy = peekChar(parser) == '?';
  if (lazy) parser->position++;

  /* The atom's code is lifted out and re-emitted as many times as the bounds
   * require, so `a{2,4}` becomes `aa a? a?`. */
  int atomLength = atomEnd - atomStart;
  ReInst *atom = (ReInst *)malloc(sizeof(ReInst) * (size_t)(atomLength > 0 ? atomLength : 1));
  memcpy(atom, regex->code + atomStart, sizeof(ReInst) * (size_t)atomLength);
  regex->count = atomStart;

  int copies = least;
  for (int i = 0; i < copies; i++) {
    int at = regex->count;
    for (int j = 0; j < atomLength; j++) {
      ReInst inst = atom[j];
      if (inst.op == RE_SPLIT || inst.op == RE_JUMP) {
        inst.x += at - atomStart;
        if (inst.op == RE_SPLIT) inst.y += at - atomStart;
      }
      emit(regex, inst.op, inst.x, inst.y);
    }
  }

  if (most == -1) {
    /* `e*` — split, body, jump back. `e+` is the same after the copies above. */
    int split = emit(regex, RE_SPLIT, 0, 0);
    int bodyStart = regex->count;
    for (int j = 0; j < atomLength; j++) {
      ReInst inst = atom[j];
      if (inst.op == RE_SPLIT || inst.op == RE_JUMP) {
        inst.x += bodyStart - atomStart;
        if (inst.op == RE_SPLIT) inst.y += bodyStart - atomStart;
      }
      emit(regex, inst.op, inst.x, inst.y);
    }
    emit(regex, RE_JUMP, split, 0);
    regex->code[split].x = lazy ? regex->count : bodyStart;
    regex->code[split].y = lazy ? bodyStart : regex->count;
  } else {
    /* A bounded upper limit becomes that many optional copies. */
    int optional = most - least;
    int *splits = (int *)malloc(sizeof(int) * (size_t)(optional > 0 ? optional : 1));
    for (int i = 0; i < optional; i++) {
      splits[i] = emit(regex, RE_SPLIT, 0, 0);
      int bodyStart = regex->count;
      for (int j = 0; j < atomLength; j++) {
        ReInst inst = atom[j];
        if (inst.op == RE_SPLIT || inst.op == RE_JUMP) {
          inst.x += bodyStart - atomStart;
          if (inst.op == RE_SPLIT) inst.y += bodyStart - atomStart;
        }
        emit(regex, inst.op, inst.x, inst.y);
      }
      regex->code[splits[i]].x = lazy ? 0 : bodyStart; /* patched below */
      regex->code[splits[i]].y = lazy ? bodyStart : 0;
    }
    for (int i = 0; i < optional; i++) {
      if (lazy) {
        regex->code[splits[i]].x = regex->count;
      } else {
        regex->code[splits[i]].y = regex->count;
      }
    }
    free(splits);
  }

  free(atom);
}

static void parseConcatenation(ReParser *parser) {
  while (!atEnd(parser) && peekChar(parser) != '|' && peekChar(parser) != ')') {
    parseRepetition(parser);
    if (parser->failed) return;
  }
}

static void parseAlternation(ReParser *parser) {
  Regex *regex = parser->regex;
  int start = regex->count;
  parseConcatenation(parser);
  if (parser->failed) return;

  while (peekChar(parser) == '|') {
    parser->position++;

    /* The left side has already been emitted, so making room for its split
     * means shifting it up by one and relocating what it holds. */
    emit(regex, RE_JUMP, 0, 0);
    memmove(regex->code + start + 1, regex->code + start,
            sizeof(ReInst) * (size_t)(regex->count - start - 1));
    for (int i = start + 1; i < regex->count; i++) {
      if (regex->code[i].op == RE_SPLIT || regex->code[i].op == RE_JUMP) {
        if (regex->code[i].x >= start) regex->code[i].x++;
        if (regex->code[i].op == RE_SPLIT && regex->code[i].y >= start) {
          regex->code[i].y++;
        }
      }
    }
    regex->code[start].op = RE_SPLIT;
    regex->code[start].x = start + 1;

    int jump = emit(regex, RE_JUMP, 0, 0);
    regex->code[start].y = regex->count;
    parseConcatenation(parser);
    if (parser->failed) return;
    regex->code[jump].x = regex->count;
  }
}

Regex *csRegexCompile(const char *pattern, int length, bool ignoreCase, bool multiline,
                      bool dotAll, char *error, size_t errorSize) {
  Regex *regex = (Regex *)calloc(1, sizeof(Regex));
  regex->groupCount = 1; /* group 0 is the whole match */
  regex->ignoreCase = ignoreCase;
  regex->multiline = multiline;
  regex->dotAll = dotAll;

  ReParser parser = {regex, pattern, length, 0, error, errorSize, false};

  emit(regex, RE_SAVE, 0, 0);
  parseAlternation(&parser);
  if (!parser.failed && !atEnd(&parser)) fail(&parser, "unexpected ')'");
  emit(regex, RE_SAVE, 1, 0);
  emit(regex, RE_MATCH, 0, 0);

  if (parser.failed) {
    csRegexFree(regex);
    return NULL;
  }
  return regex;
}
