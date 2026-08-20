/* regex.c — the pattern compiler and the backtracking matcher.
 *
 * See regex.h for why backtracking rather than an NFA simulation. The shape is
 * the classic one: a recursive-descent parse of the pattern emitting
 * instructions as it goes, then a VM that walks them with an explicit stack.
 *
 *   alternation = concatenation ( "|" concatenation )*
 *   concatenation = repetition*
 *   repetition  = atom ( "*" | "+" | "?" | "{n,m}" ) "?"?
 *   atom        = "(" alternation ")" | "[" class "]" | "." | escape | literal
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/regex.h"

typedef enum {
  RE_CHAR,          /* one specific byte                       */
  RE_ANY,           /* `.`                                     */
  RE_CLASS,         /* a 256-bit set, indexed by `x`           */
  RE_SPLIT,         /* try `x`, and on failure `y`             */
  RE_JUMP,
  RE_SAVE,          /* record the current position in slot `x` */
  RE_ASSERT_BOL,
  RE_ASSERT_EOL,
  RE_ASSERT_WORD,   /* \b, or \B when `x` is 0                 */
  /* `\1`..`\9`. Only a backtracker can have these: what they match is not
   * known until the group they name has matched, so no NFA simulation over a
   * fixed alphabet can express one. */
  RE_BACKREF,       /* the text group `x` captured             */
  /* `(?=…)` and `(?!…)`. `x` is 1 when negated; `y` is where to continue when
   * the assertion holds. The body runs as a sub-match that consumes nothing. */
  RE_LOOK,
  RE_LOOK_END,      /* the end of a lookahead body             */
  RE_MATCH,
} ReOp;

typedef struct {
  ReOp op;
  int x;
  int y;
} ReInst;

typedef struct {
  uint8_t bits[32]; /* one bit per byte value */
} ReClass;

struct Regex {
  ReInst *code;
  int count;
  int capacity;

  ReClass *classes;
  int classCount;
  int classCapacity;

  int groupCount; /* including group 0 */
  bool ignoreCase;
  bool multiline;
  bool dotAll;
};

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

static bool classHas(const ReClass *set, unsigned char byte) {
  return (set->bits[byte >> 3] & (1u << (byte & 7))) != 0;
}

static void classAddRange(ReClass *set, unsigned char from, unsigned char to) {
  for (int c = from; c <= (int)to; c++) classAdd(set, (unsigned char)c);
}

static bool isWordByte(unsigned char c) {
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
      if (classHas(&set, (unsigned char)c)) classAdd(&set, (unsigned char)(c - 32));
    }
    for (int c = 'A'; c <= 'Z'; c++) {
      if (classHas(&set, (unsigned char)c)) classAdd(&set, (unsigned char)(c + 32));
    }
  }
  if (negated) negate(&set);

  emit(parser->regex, RE_CLASS, addClass(parser->regex, &set), 0);
}

/* Parses one atom, returning where its code starts so a quantifier can wrap it. */
static int parseAtom(ReParser *parser) {
  Regex *regex = parser->regex;
  int start = regex->count;
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
        } else if (kind == '<') {
          /* Lookbehind has to run the body backwards from each position, which
           * this engine has no way to do; naming it beats a generic error. */
          fail(parser, "lookbehind and named groups are not supported");
          return start;
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

void csRegexFree(Regex *regex) {
  if (regex == NULL) return;
  free(regex->code);
  free(regex->classes);
  free(regex);
}

int csRegexGroupCount(const Regex *regex) { return regex->groupCount; }

/* ---- the matcher ------------------------------------------------------- */

/* A pending alternative, plus how far the capture trail had grown when it was
 * pushed — unwinding to that point is what undoes the saves made since. */
typedef struct {
  int pc;
  int sp;
  int trail;
} Thread;

typedef struct {
  int slot;
  int value;
} TrailEntry;

/* Case-folded byte equality, for a backreference under `i`. Every other
 * comparison folds at compile time; this one cannot, because what it compares
 * against is not known until the group has matched. */
static bool sameByte(unsigned char a, unsigned char b, bool ignoreCase) {
  if (a == b) return true;
  if (!ignoreCase) return false;
  if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
  return a == b;
}

/* Runs the program from `pc` at `sp`, backtracking over its own alternatives.
 *
 * Separated from the leftmost scan because a lookahead runs the same machine
 * on a slice of the same program: it recurses here, and the sub-match's
 * alternatives live on its own stack rather than escaping into the caller's,
 * which is what makes an assertion consume nothing.
 *
 * `stopOp` is what counts as success — RE_MATCH for the whole pattern, and
 * RE_LOOK_END for a lookahead body. */
static bool matchFrom(const Regex *regex, const char *subject, int length,
                      int pc, int sp, int *slots, long *steps, bool *exhausted,
                      ReOp stopOp, int *endSp) {
  Thread threadsInline[64];
  Thread *threads = threadsInline;
  int threadCapacity = 64;
  int threadCount = 0;

  TrailEntry trailInline[64];
  TrailEntry *trail = trailInline;
  int trailCapacity = 64;
  int trailCount = 0;

  bool matched = false;

  for (;;) {
    if (++*steps > CS_REGEX_MAX_STEPS) {
      *exhausted = true;
      break;
    }

    bool alive = true;
    const ReInst *inst = &regex->code[pc];

    switch (inst->op) {
      case RE_CHAR:
        alive = sp < length && (unsigned char)subject[sp] == (unsigned char)inst->x;
        if (alive) sp++;
        pc++;
        break;

      case RE_ANY:
        alive = sp < length && (regex->dotAll || subject[sp] != '\n');
        if (alive) sp++;
        pc++;
        break;

      case RE_CLASS:
        alive = sp < length &&
                classHas(&regex->classes[inst->x], (unsigned char)subject[sp]);
        if (alive) sp++;
        pc++;
        break;

      case RE_BACKREF: {
        int from = slots[inst->x * 2];
        int to = slots[inst->x * 2 + 1];
        /* A group that has not matched yet stands for the empty string, which
         * is what JavaScript says and what makes `/(a)?\1/` match "". */
        if (from < 0 || to < 0) {
          pc++;
          break;
        }
        int span = to - from;
        alive = sp + span <= length;
        for (int i = 0; alive && i < span; i++) {
          alive = sameByte((unsigned char)subject[sp + i],
                           (unsigned char)subject[from + i], regex->ignoreCase);
        }
        if (alive) sp += span;
        pc++;
        break;
      }

      case RE_LOOK: {
        /* The body decides whether to go on, and never moves the cursor. A
         * failed assertion must also leave no captures behind, so the slots
         * are put back when it does not hold. */
        int saved[CS_REGEX_MAX_GROUPS * 2];
        memcpy(saved, slots, sizeof(int) * (size_t)(regex->groupCount * 2));

        int ignored = sp;
        bool held = matchFrom(regex, subject, length, pc + 1, sp, slots, steps,
                              exhausted, RE_LOOK_END, &ignored);
        if (*exhausted) { alive = false; break; }

        bool wanted = inst->x == 0;
        alive = held == wanted;
        if (!held || !wanted) {
          memcpy(slots, saved, sizeof(int) * (size_t)(regex->groupCount * 2));
        }
        pc = inst->y;
        break;
      }

      case RE_ASSERT_BOL:
        alive = sp == 0 || (regex->multiline && subject[sp - 1] == '\n');
        pc++;
        break;

      case RE_ASSERT_EOL:
        alive = sp == length || (regex->multiline && subject[sp] == '\n');
        pc++;
        break;

      case RE_ASSERT_WORD: {
        bool before = sp > 0 && isWordByte((unsigned char)subject[sp - 1]);
        bool after = sp < length && isWordByte((unsigned char)subject[sp]);
        alive = (before != after) == (inst->x == 1);
        pc++;
        break;
      }

      case RE_SAVE:
        if (trailCount + 1 > trailCapacity) {
          int grown = trailCapacity * 2;
          TrailEntry *moved = (TrailEntry *)malloc(sizeof(TrailEntry) * (size_t)grown);
          memcpy(moved, trail, sizeof(TrailEntry) * (size_t)trailCount);
          if (trail != trailInline) free(trail);
          trail = moved;
          trailCapacity = grown;
        }
        trail[trailCount].slot = inst->x;
        trail[trailCount].value = slots[inst->x];
        trailCount++;
        slots[inst->x] = sp;
        pc++;
        break;

      case RE_SPLIT:
        if (threadCount + 1 > threadCapacity) {
          int grown = threadCapacity * 2;
          Thread *moved = (Thread *)malloc(sizeof(Thread) * (size_t)grown);
          memcpy(moved, threads, sizeof(Thread) * (size_t)threadCount);
          if (threads != threadsInline) free(threads);
          threads = moved;
          threadCapacity = grown;
        }
        threads[threadCount].pc = inst->y;
        threads[threadCount].sp = sp;
        threads[threadCount].trail = trailCount;
        threadCount++;
        pc = inst->x;
        break;

      case RE_JUMP:
        pc = inst->x;
        break;

      case RE_LOOK_END:
      case RE_MATCH:
        if (inst->op == stopOp) {
          matched = true;
        } else {
          /* Running off the end of a lookahead body into the pattern that
           * follows it, or the reverse, would be a compiler bug — but failing
           * is the safe answer rather than reading past the program. */
          alive = false;
        }
        break;
    }

    if (matched) break;

    if (!alive) {
      /* Backtrack: take the most recent alternative and undo every capture
       * recorded after it was pushed. */
      if (threadCount == 0) break;
      threadCount--;
      pc = threads[threadCount].pc;
      sp = threads[threadCount].sp;
      while (trailCount > threads[threadCount].trail) {
        trailCount--;
        slots[trail[trailCount].slot] = trail[trailCount].value;
      }
    }
  }

  if (threads != threadsInline) free(threads);
  if (trail != trailInline) free(trail);
  *endSp = sp;
  return matched;
}

bool csRegexSearch(const Regex *regex, const char *subject, int length, int start,
                   RegexMatch *match, bool *outOfSteps) {
  if (outOfSteps != NULL) *outOfSteps = false;

  int slots[CS_REGEX_MAX_GROUPS * 2];
  long steps = 0;
  bool matched = false;
  bool exhausted = false;

  /* Leftmost: try each starting position in turn, and the first that matches
   * wins — which is what makes `/a|ab/` on "ab" match just "a". */
  for (int at = start; at <= length && !matched && !exhausted; at++) {
    for (int i = 0; i < regex->groupCount * 2; i++) slots[i] = -1;

    int endSp = at;
    matched = matchFrom(regex, subject, length, 0, at, slots, &steps, &exhausted,
                        RE_MATCH, &endSp);

    if (matched) {
      match->groupCount = regex->groupCount;
      for (int g = 0; g < regex->groupCount; g++) {
        match->groups[g].start = slots[g * 2];
        match->groups[g].end = slots[g * 2 + 1];
      }
    }
  }

  if (exhausted && outOfSteps != NULL) *outOfSteps = true;
  return matched;
}
