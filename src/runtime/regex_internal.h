/* regex_internal.h — the compiled program, shared by the compiler and the
 * matcher.
 *
 * The engine is deliberately independent of the language: the interface is a
 * pattern in and a program out, and a matcher over that program.
 * tests/regex_engine_test.c links regex.c on its own and exercises it without
 * a VM, which is how it was written and how a change to it is checked first.
 */
#ifndef CSCRIPT_RUNTIME_REGEX_INTERNAL_H
#define CSCRIPT_RUNTIME_REGEX_INTERNAL_H

#include "cscript/regex.h"

typedef enum {
  RE_CHAR,  /* one specific byte                       */
  RE_ANY,   /* `.`                                     */
  RE_CLASS, /* a 256-bit set, indexed by `x`           */
  RE_SPLIT, /* try `x`, and on failure `y`             */
  RE_JUMP,
  RE_SAVE, /* record the current position in slot `x` */
  RE_ASSERT_BOL,
  RE_ASSERT_EOL,
  RE_ASSERT_WORD, /* \b, or \B when `x` is 0                 */
  /* `\1`..`\9`. Only a backtracker can have these: what they match is not
   * known until the group they name has matched, so no NFA simulation over a
   * fixed alphabet can express one. */
  RE_BACKREF, /* the text group `x` captured             */
  /* `(?=…)` and `(?!…)`. `x` is 1 when negated; `y` is where to continue when
   * the assertion holds. The body runs as a sub-match that consumes nothing. */
  RE_LOOK,
  /* `(?<=…)` and `(?<!…)`. The body is the same shape as a lookahead's, but it
   * has to match text *ending* where the cursor is rather than starting there.
   * This engine cannot run a program backwards, so it does the other thing
   * that gives the same answer: try every start position at or before the
   * cursor and require the body to finish exactly at it. That is linear in how
   * far back it looks, which for the assertions people write is a handful of
   * bytes — and it is correct for a body of any shape, which a width
   * calculation would not be. */
  RE_LOOKBEHIND,
  RE_LOOK_END, /* the end of a lookahead or lookbehind body */
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

typedef struct {
  char *name; /* NUL-terminated, owned */
  int group;
} ReGroupName;

struct Regex {
  ReInst *code;
  int count;
  int capacity;

  ReClass *classes;
  int classCount;
  int classCapacity;

  int groupCount; /* including group 0 */

  /* `(?<name>…)`. A named group is an ordinary capturing group that also
   * answers to a name, so only the mapping is new — the matching is not. */
  ReGroupName *names;
  int nameCount;
  int nameCapacity;

  bool ignoreCase;
  bool multiline;
  bool dotAll;
};

/* True when the byte is a word character, which `\b` and `\w` both need. */
bool csRegexIsWordByte(unsigned char c);
bool csRegexClassHas(const ReClass *set, unsigned char byte);

#endif /* CSCRIPT_RUNTIME_REGEX_INTERNAL_H */
