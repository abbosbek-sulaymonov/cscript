#include <stdio.h>
#include <string.h>
#include "cscript/regex.h"

static int failures = 0;

static void check(const char *pattern, const char *subject, const char *expect) {
  char error[128];
  Regex *re = csRegexCompile(pattern, (int)strlen(pattern), false, false, false,
                             error, sizeof error);
  if (re == NULL) { printf("FAIL /%s/ compile: %s\n", pattern, error); failures++; return; }

  RegexMatch m;
  bool over = false;
  char got[256] = "-";
  if (csRegexSearch(re, subject, (int)strlen(subject), 0, &m, &over)) {
    int n = m.groups[0].end - m.groups[0].start;
    snprintf(got, sizeof got, "%.*s", n, subject + m.groups[0].start);
  }
  if (strcmp(got, expect) != 0) {
    printf("FAIL /%s/ on \"%s\": got \"%s\" want \"%s\"\n", pattern, subject, got, expect);
    failures++;
  }
  csRegexFree(re);
}

static void group(const char *pattern, const char *subject, int g, const char *expect) {
  char error[128];
  Regex *re = csRegexCompile(pattern, (int)strlen(pattern), false, false, false,
                             error, sizeof error);
  RegexMatch m;
  char got[256] = "-";
  if (re && csRegexSearch(re, subject, (int)strlen(subject), 0, &m, NULL)) {
    if (m.groups[g].start >= 0) {
      snprintf(got, sizeof got, "%.*s", m.groups[g].end - m.groups[g].start,
               subject + m.groups[g].start);
    }
  }
  if (strcmp(got, expect) != 0) {
    printf("FAIL /%s/ group %d on \"%s\": got \"%s\" want \"%s\"\n", pattern, g, subject, got, expect);
    failures++;
  }
  csRegexFree(re);
}

int main(void) {
  check("abc", "xxabcyy", "abc");
  check("a+", "caaab", "aaa");
  check("a*", "bbb", "");
  check("a+?", "caaab", "a");
  check("a?b", "ab", "ab");
  check(".", "\nx", "x");
  check("^abc", "abcd", "abc");
  check("^abc", "xabc", "-");
  check("c$", "abc", "c");
  check("[a-c]+", "xxabcabz", "abcab");
  check("[^a-c]+", "abcxyz", "xyz");
  check("\\d+", "ab123cd", "123");
  check("\\w+", "  hi_there!", "hi_there");
  check("\\s+", "ab   cd", "   ");
  check("\\D+", "12ab34", "ab");
  check("a|b", "zzb", "b");
  check("abc|abd", "xabd", "abd");
  check("(ab)+", "ababab", "ababab");
  check("(a|b)*c", "ababc", "ababc");
  check("a{3}", "aaaa", "aaa");
  check("a{2,3}", "aaaa", "aaa");
  check("a{2,}", "aaaaa", "aaaaa");
  check("colou?r", "color", "color");
  check("colou?r", "colour", "colour");
  check("\\bword\\b", "a word here", "word");
  check("\\bword\\b", "sword", "-");
  check("(?:ab)+", "abab", "abab");
  check("[.]", "a.b", ".");
  check("\\.", "a.b", ".");
  check("a\\/b", "a/b", "a/b");
  check("<[^>]+>", "x<tag attr>y", "<tag attr>");
  group("(\\d+)-(\\d+)", "x12-34y", 1, "12");
  group("(\\d+)-(\\d+)", "x12-34y", 2, "34");
  group("(a)?b", "b", 1, "-");
  group("(a+)(b+)", "aabbb", 2, "bbb");
  group("^(\\w+)\\s+(\\w+)$", "hello world", 2, "world");

  /* Lookbehind. The engine cannot run a program backwards, so it tries every
   * start position at or before the cursor and requires the body to end
   * exactly there — which is why a body of any shape works, not just a fixed
   * width one. */
  check("(?<=\\$)\\d+", "price: $30", "30");
  check("(?<=\\$)\\d+", "cost 40", "-");
  check("(?<!x)a", "xay", "-");
  check("(?<!x)a", "yay", "a");
  check("(?<=a+)b", "aaab", "b");
  check("(?<=c|z)ab", "cab", "ab");
  check("(?<=^)a", "abc", "a");
  check("(?<=ab)c", "abc", "c");
  check("(?<=ab)c", "axc", "-");
  /* An empty body holds everywhere, and a negated one therefore never does. */
  check("(?<=)a", "a", "a");
  check("(?<!)a", "a", "-");

  /* A named group is an ordinary capturing group that also answers to a name,
   * so it captures by number exactly as it did before. */
  group("(?<y>\\d{4})-(?<m>\\d\\d)", "2024-01", 1, "2024");
  group("(?<y>\\d{4})-(?<m>\\d\\d)", "2024-01", 2, "01");
  group("(?<a>x)?(?<b>y)", "y", 1, "-");

  char error[128];
  Regex *bad = csRegexCompile("(a", 2, false, false, false, error, sizeof error);
  if (bad != NULL) { printf("FAIL: unterminated group compiled\n"); failures++; }

  Regex *unterminated =
      csRegexCompile("(?<name", 7, false, false, false, error, sizeof error);
  if (unterminated != NULL) {
    printf("FAIL: unterminated group name compiled\n");
    failures++;
  }

  Regex *duplicate = csRegexCompile("(?<n>a)(?<n>b)", 14, false, false, false,
                                    error, sizeof error);
  if (duplicate != NULL) { printf("FAIL: duplicate group name compiled\n"); failures++; }

  Regex *named = csRegexCompile("(?<y>\\d+)", 10, false, false, false, error,
                                sizeof error);
  if (named == NULL) {
    printf("FAIL: named group did not compile: %s\n", error);
    failures++;
  } else {
    int group = -1;
    if (csRegexNameCount(named) != 1 ||
        strcmp(csRegexNameAt(named, 0, &group), "y") != 0 || group != 1) {
      printf("FAIL: named group not recorded\n");
      failures++;
    }
    if (csRegexGroupNamed(named, "y", 1) != 1 ||
        csRegexGroupNamed(named, "z", 1) != -1) {
      printf("FAIL: lookup by name is wrong\n");
      failures++;
    }
    csRegexFree(named);
  }

  printf(failures ? "\n%d failures\n" : "\nall regex engine tests passed\n", failures);
  return failures != 0;
}
