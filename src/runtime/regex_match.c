/* regex_match.c — running a compiled pattern against a subject.
 *
 * Backtracking, with an explicit trail rather than the C stack: a pattern like
 * `(a*)*b` can need more backtracking than a recursion depth would survive,
 * and a regex is user input. The trail is also what makes the group captures
 * restorable — a failed alternative has to undo the groups it set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/regex.h"

#include "runtime/regex_internal.h"

void csRegexFree(Regex *regex) {
  if (regex == NULL) return;
  free(regex->code);
  free(regex->classes);
  for (int i = 0; i < regex->nameCount; i++) free(regex->names[i].name);
  free(regex->names);
  free(regex);
}

int csRegexGroupCount(const Regex *regex) {
  return regex->groupCount;
}

int csRegexNameCount(const Regex *regex) {
  return regex->nameCount;
}

const char *csRegexNameAt(const Regex *regex, int index, int *group) {
  *group = regex->names[index].group;
  return regex->names[index].name;
}

int csRegexGroupNamed(const Regex *regex, const char *name, int length) {
  for (int i = 0; i < regex->nameCount; i++) {
    if ((int)strlen(regex->names[i].name) == length && memcmp(regex->names[i].name, name, (size_t)length) == 0) {
      return regex->names[i].group;
    }
  }
  return -1;
}

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
static bool matchFrom(const Regex *regex, const char *subject, int length, int pc, int sp, int *slots, long *steps, bool *exhausted, ReOp stopOp, int *endSp) {
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
        alive = sp < length && csRegexClassHas(&regex->classes[inst->x], (unsigned char)subject[sp]);
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
          alive = sameByte((unsigned char)subject[sp + i], (unsigned char)subject[from + i], regex->ignoreCase);
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
        bool held = matchFrom(regex, subject, length, pc + 1, sp, slots, steps, exhausted, RE_LOOK_END, &ignored);
        if (*exhausted) {
          alive = false;
          break;
        }

        bool wanted = inst->x == 0;
        alive = held == wanted;
        if (!held || !wanted) {
          memcpy(slots, saved, sizeof(int) * (size_t)(regex->groupCount * 2));
        }
        pc = inst->y;
        break;
      }

      case RE_LOOKBEHIND: {
        /* Every start position at or before the cursor, nearest first, and the
         * body must finish exactly here. Nearest first because a lookbehind is
         * usually short, so the answer is usually found immediately. */
        int saved[CS_REGEX_MAX_GROUPS * 2];
        memcpy(saved, slots, sizeof(int) * (size_t)(regex->groupCount * 2));

        bool held = false;
        for (int from = sp; from >= 0 && !held; from--) {
          int ended = from;
          if (!matchFrom(regex, subject, length, pc + 1, from, slots, steps, exhausted, RE_LOOK_END, &ended)) {
            if (*exhausted) break;
            memcpy(slots, saved, sizeof(int) * (size_t)(regex->groupCount * 2));
            continue;
          }
          /* Matched, but only counts if it ends where the cursor is. */
          held = ended == sp;
          if (!held) {
            memcpy(slots, saved, sizeof(int) * (size_t)(regex->groupCount * 2));
          }
        }
        if (*exhausted) {
          alive = false;
          break;
        }

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
        bool before = sp > 0 && csRegexIsWordByte((unsigned char)subject[sp - 1]);
        bool after = sp < length && csRegexIsWordByte((unsigned char)subject[sp]);
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

      case RE_JUMP: pc = inst->x; break;

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

bool csRegexSearch(const Regex *regex, const char *subject, int length, int start, RegexMatch *match, bool *outOfSteps) {
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
    matched = matchFrom(regex, subject, length, 0, at, slots, &steps, &exhausted, RE_MATCH, &endSp);

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
