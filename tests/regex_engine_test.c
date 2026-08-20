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

  char error[128];
  Regex *bad = csRegexCompile("(a", 2, false, false, false, error, sizeof error);
  if (bad != NULL) { printf("FAIL: unterminated group compiled\n"); failures++; }

  printf(failures ? "\n%d failures\n" : "\nall regex engine tests passed\n", failures);
  return failures != 0;
}
