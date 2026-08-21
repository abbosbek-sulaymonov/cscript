/* regex.h — regular expressions: a pattern compiler and a matcher.
 *
 * A pattern is compiled to a small instruction set of its own and run by a
 * backtracking virtual machine. That is the design JavaScript's semantics
 * require: leftmost-first alternation, greedy and lazy quantifiers, and
 * capture groups that report where they last matched are all *definitions in
 * terms of backtracking order*. A Thompson NFA simulation is asymptotically
 * better and cannot express them.
 *
 * The price is that a pattern can be made to take exponential time — the
 * `(a+)+b` family. The matcher counts the steps it takes and gives up rather
 * than hanging, which turns a denial of service into an error message.
 *
 * The backtracking stack is explicit rather than the C stack, so a long
 * subject cannot overflow it, and so the step budget is the only limit.
 */
#ifndef CSCRIPT_REGEX_H
#define CSCRIPT_REGEX_H

#include "cscript/common.h"

#define CS_REGEX_MAX_GROUPS 32
#define CS_REGEX_MAX_STEPS 1000000

typedef struct Regex Regex;

typedef struct {
  int start; /* byte offset, or -1 when the group did not participate */
  int end;
} RegexGroup;

typedef struct {
  RegexGroup groups[CS_REGEX_MAX_GROUPS];
  int groupCount; /* including group 0, the whole match */
} RegexMatch;

/* Compiles `pattern`. Returns NULL and writes a reason into `error` — which
 * must have room for at least 128 bytes — when the pattern is malformed or
 * uses something unsupported. */
Regex *csRegexCompile(const char *pattern, int length, bool ignoreCase,
                      bool multiline, bool dotAll, char *error, size_t errorSize);

void csRegexFree(Regex *regex);

/* How many capture groups the pattern has, group 0 included. */
int csRegexGroupCount(const Regex *regex);

/* `(?<name>…)` — how many groups have names, what they are, and which group a
 * name belongs to. `csRegexGroupNamed` answers -1 for a name nothing has. */
int csRegexNameCount(const Regex *regex);
const char *csRegexNameAt(const Regex *regex, int index, int *group);
int csRegexGroupNamed(const Regex *regex, const char *name, int length);

/* Searches `subject` from `start`. Returns true and fills `match` when the
 * pattern matches somewhere at or after `start`.
 *
 * `outOfSteps` is set when the matcher gave up rather than failing, so a
 * caller can tell "no match" from "this pattern is pathological". */
bool csRegexSearch(const Regex *regex, const char *subject, int length, int start,
                   RegexMatch *match, bool *outOfSteps);

#endif /* CSCRIPT_REGEX_H */
