/* native_regex.c — what a regular expression object can do, and the string
 * methods that take one.
 *
 * The engine is in regex.c; this is the part that knows JavaScript's rules
 * about it — which are mostly about `lastIndex` and the `g` flag, and which
 * are the part people actually get wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/regex.h"
#include "cscript/vm.h"

static bool requireRegex(Value receiver, const char *method) {
  if (IS_REGEX(receiver)) return true;
  csVMRuntimeError("'%s' can only be called on a regular expression", method);
  return false;
}

/* Runs the pattern, reporting a pathological one rather than hanging. */
static bool search(ObjRegex *regex, const char *subject, int length, int from,
                   RegexMatch *match, bool *found) {
  bool outOfSteps = false;
  *found = csRegexSearch(regex->program, subject, length, from, match, &outOfSteps);
  if (outOfSteps) {
    csVMRuntimeError("regular expression /%s/ took too long on this input",
                     regex->source->chars);
    return false;
  }
  return true;
}

/* The array `exec` and `match` produce: the whole match, then each group, with
 * `index` and `input` hung off it as properties — which is what makes it an
 * array in JavaScript rather than a record. */
static ObjArray *buildMatchArray(const RegexMatch *match, const char *subject) {
  ObjArray *result = csArrayNew();
  csPushTempRoot((Obj *)result);

  for (int g = 0; g < match->groupCount; g++) {
    Value element = UNDEFINED_VAL;
    if (match->groups[g].start >= 0) {
      ObjString *text = csStringCopy(subject + match->groups[g].start,
                                     match->groups[g].end - match->groups[g].start);
      element = OBJ_VAL(text);
    }
    if (IS_OBJ(element)) csPushTempRoot(AS_OBJ(element));
    csValueArrayWrite(&result->elements, element);
    if (IS_OBJ(element)) csPopTempRoot();
  }

  csPopTempRoot();
  return result;
}

static bool regexTest(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireRegex(receiver, "test")) return false;
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("test expects a string");
    return false;
  }
  ObjRegex *regex = AS_REGEX(receiver);
  ObjString *subject = AS_STRING(args[0]);

  /* A global pattern resumes where it left off, and resets when it runs out.
   * This is JavaScript's most surprising rule and it is faithfully kept. */
  int from = regex->global ? regex->lastIndex : 0;
  if (from > subject->length) {
    regex->lastIndex = 0;
    *result = BOOL_VAL(false);
    return true;
  }

  RegexMatch match;
  bool found;
  if (!search(regex, subject->chars, subject->length, from, &match, &found)) return false;

  if (regex->global) {
    regex->lastIndex = found ? match.groups[0].end : 0;
    /* An empty match would otherwise never advance. */
    if (found && match.groups[0].end == match.groups[0].start) regex->lastIndex++;
  }
  *result = BOOL_VAL(found);
  return true;
}

static bool regexExec(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireRegex(receiver, "exec")) return false;
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("exec expects a string");
    return false;
  }
  ObjRegex *regex = AS_REGEX(receiver);
  ObjString *subject = AS_STRING(args[0]);

  int from = regex->global ? regex->lastIndex : 0;
  if (from > subject->length) {
    regex->lastIndex = 0;
    *result = NULL_VAL;
    return true;
  }

  RegexMatch match;
  bool found;
  if (!search(regex, subject->chars, subject->length, from, &match, &found)) return false;
  if (!found) {
    if (regex->global) regex->lastIndex = 0;
    *result = NULL_VAL;
    return true;
  }

  if (regex->global) {
    regex->lastIndex = match.groups[0].end;
    if (match.groups[0].end == match.groups[0].start) regex->lastIndex++;
  }

  ObjArray *array = buildMatchArray(&match, subject->chars);
  csPushTempRoot((Obj *)array);
  /* `index` and `input`, as JavaScript hangs them off the result. An array
   * carries no properties until one is put on it, so an ordinary array pays
   * nothing for these. */
  csArrayPutExtra(array, "index", 5, NUMBER_VAL(match.groups[0].start));
  csArrayPutExtra(array, "input", 5, OBJ_VAL(subject));
  csPopTempRoot();
  *result = OBJ_VAL(array);
  return true;
}

static void defineRegexMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.regexMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csRegexMethodsInstall(void) {
  defineRegexMethod("test", regexTest, -1);
  defineRegexMethod("exec", regexExec, -1);
}

/* ---- the string side --------------------------------------------------- */

/* Everything below takes a pattern where a string method would take a string,
 * and each is written once against the engine rather than once per method. */

bool csRegexStringSearch(Value receiver, int argCount, Value *args, Value *result) {
  if (!IS_STRING(receiver) || argCount < 1 || !IS_REGEX(args[0])) return false;
  ObjString *subject = AS_STRING(receiver);
  ObjRegex *regex = AS_REGEX(args[0]);

  RegexMatch match;
  bool found;
  if (!search(regex, subject->chars, subject->length, 0, &match, &found)) return false;
  *result = NUMBER_VAL(found ? match.groups[0].start : -1);
  return true;
}

bool csRegexStringMatch(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *subject = AS_STRING(receiver);
  ObjRegex *regex = AS_REGEX(args[0]);
  (void)argCount;

  if (!regex->global) {
    RegexMatch match;
    bool found;
    if (!search(regex, subject->chars, subject->length, 0, &match, &found)) return false;
    if (!found) {
      *result = NULL_VAL;
      return true;
    }
    /* Without `g`, `match` answers what `exec` would — including where. */
    ObjArray *array = buildMatchArray(&match, subject->chars);
    csPushTempRoot((Obj *)array);
    csArrayPutExtra(array, "index", 5, NUMBER_VAL(match.groups[0].start));
    csArrayPutExtra(array, "input", 5, OBJ_VAL(subject));
    csPopTempRoot();
    *result = OBJ_VAL(array);
    return true;
  }

  /* With `g`, every match rather than the groups of the first one. */
  ObjArray *all = csArrayNew();
  csPushTempRoot((Obj *)all);

  int from = 0;
  for (;;) {
    RegexMatch match;
    bool found;
    if (!search(regex, subject->chars, subject->length, from, &match, &found)) {
      csPopTempRoot();
      return false;
    }
    if (!found) break;

    ObjString *text = csStringCopy(subject->chars + match.groups[0].start,
                                   match.groups[0].end - match.groups[0].start);
    csPushTempRoot((Obj *)text);
    csValueArrayWrite(&all->elements, OBJ_VAL(text));
    csPopTempRoot();

    from = match.groups[0].end;
    if (match.groups[0].end == match.groups[0].start) from++;
    if (from > subject->length) break;
  }

  regex->lastIndex = 0;
  csPopTempRoot();
  *result = all->elements.count > 0 ? OBJ_VAL(all) : NULL_VAL;
  return true;
}

/* Calls a replacer with what JavaScript hands one: the whole match, then each
 * capture, then where it started, then the subject.
 *
 * The callee is user code, so it can allocate and collect. Every string built
 * here is rooted across the call, and the caller's output buffer is plain
 * malloc rather than GC memory, so nothing it holds can move. */
static bool callReplacer(Value replacer, ObjString *subject,
                         const RegexMatch *match, ObjString **out) {
  Value argv[16];
  int argc = 0;
  int rooted = 0;

  ObjString *whole = csStringCopy(subject->chars + match->groups[0].start,
                                  match->groups[0].end - match->groups[0].start);
  csPushTempRoot((Obj *)whole);
  rooted++;
  argv[argc++] = OBJ_VAL(whole);

  for (int i = 1; i < match->groupCount && argc < 14; i++) {
    if (match->groups[i].start < 0) {
      argv[argc++] = UNDEFINED_VAL;
      continue;
    }
    ObjString *piece = csStringCopy(subject->chars + match->groups[i].start,
                                    match->groups[i].end - match->groups[i].start);
    csPushTempRoot((Obj *)piece);
    rooted++;
    argv[argc++] = OBJ_VAL(piece);
  }

  argv[argc++] = NUMBER_VAL(match->groups[0].start);
  argv[argc++] = OBJ_VAL(subject);

  Value produced;
  bool ok = csVMCallAdapted(replacer, argv, argc, &produced);
  for (int i = 0; i < rooted; i++) csPopTempRoot();
  if (!ok) return false;

  /* Whatever it returned becomes text, as it does in JavaScript. */
  if (IS_STRING(produced)) {
    *out = AS_STRING(produced);
    return true;
  }
  size_t length = 0;
  char *text = csValueToCString(produced, &length);
  if (text == NULL) {
    csVMRuntimeError("out of memory building a replacement");
    return false;
  }
  *out = csStringCopy(text, (int)length);
  free(text);
  return true;
}

/* `replace` and `replaceAll` with a pattern.
 *
 * The replacement is either a string, in which case `$1`..`$9` and `$&` stand
 * for the groups and the whole match and `$$` is a literal dollar, or a
 * function called once per match with what it matched. */
bool csRegexStringReplace(Value receiver, int argCount, Value *args, Value *result,
                          bool all) {
  ObjString *subject = AS_STRING(receiver);
  ObjRegex *regex = AS_REGEX(args[0]);

  bool byFunction = argCount >= 2 && csValueIsCallable(args[1]);
  if (argCount < 2 || (!IS_STRING(args[1]) && !byFunction)) {
    csVMRuntimeError("replacing with a pattern needs a string or a function");
    return false;
  }
  ObjString *replacement = byFunction ? NULL : AS_STRING(args[1]);

  /* `replace` with a `g` pattern replaces every match, which is the one place
   * the flag changes what a method does rather than where it resumes. */
  bool everywhere = all || regex->global;

  size_t capacity = (size_t)subject->length + 16;
  char *out = (char *)malloc(capacity);
  size_t length = 0;
  int from = 0;

  for (;;) {
    RegexMatch match;
    bool found;
    if (!search(regex, subject->chars, subject->length, from, &match, &found)) {
      free(out);
      return false;
    }
    if (!found) break;

    size_t needed = length + (size_t)(match.groups[0].start - from) +
                    (size_t)(replacement != NULL ? replacement->length : 0) +
                    (size_t)subject->length;
    if (needed > capacity) {
      capacity = needed * 2;
      out = (char *)realloc(out, capacity);
    }

    memcpy(out + length, subject->chars + from,
           (size_t)(match.groups[0].start - from));
    length += (size_t)(match.groups[0].start - from);

    if (byFunction) {
      ObjString *produced = NULL;
      if (!callReplacer(args[1], subject, &match, &produced)) {
        free(out);
        return false;
      }
      if (length + (size_t)produced->length + 1 > capacity) {
        capacity = (length + (size_t)produced->length + 1) * 2;
        out = (char *)realloc(out, capacity);
      }
      memcpy(out + length, produced->chars, (size_t)produced->length);
      length += (size_t)produced->length;
      goto advance;
    }

    for (int i = 0; i < replacement->length; i++) {
      char c = replacement->chars[i];
      if (c != '$' || i + 1 >= replacement->length) {
        out[length++] = c;
        continue;
      }
      char next = replacement->chars[i + 1];
      if (next == '$') {
        out[length++] = '$';
        i++;
      } else if (next == '&') {
        int span = match.groups[0].end - match.groups[0].start;
        memcpy(out + length, subject->chars + match.groups[0].start, (size_t)span);
        length += (size_t)span;
        i++;
      } else if (next >= '1' && next <= '9') {
        int group = next - '0';
        if (group < match.groupCount && match.groups[group].start >= 0) {
          int span = match.groups[group].end - match.groups[group].start;
          memcpy(out + length, subject->chars + match.groups[group].start, (size_t)span);
          length += (size_t)span;
        }
        i++;
      } else {
        out[length++] = c;
      }
    }

  advance:;
    int next = match.groups[0].end;
    if (next == match.groups[0].start) {
      /* An empty match must still advance, or this never terminates. */
      if (next < subject->length) out[length++] = subject->chars[next];
      next++;
    }
    from = next;

    if (!everywhere || from > subject->length) break;
  }

  if (from <= subject->length) {
    size_t rest = (size_t)(subject->length - from);
    if (length + rest + 1 > capacity) {
      capacity = length + rest + 1;
      out = (char *)realloc(out, capacity);
    }
    memcpy(out + length, subject->chars + from, rest);
    length += rest;
  }

  if (regex->global) regex->lastIndex = 0;
  *result = OBJ_VAL(csStringCopy(out, (int)length));
  free(out);
  return true;
}

/* `split` on a pattern. The separator's captures are *not* included, which is
 * a simplification of JavaScript's rule and is documented as one. */
bool csRegexStringSplit(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *subject = AS_STRING(receiver);
  ObjRegex *regex = AS_REGEX(args[0]);
  (void)argCount;

  ObjArray *parts = csArrayNew();
  csPushTempRoot((Obj *)parts);

  int from = 0;
  int pieceStart = 0;
  for (;;) {
    RegexMatch match;
    bool found;
    if (!search(regex, subject->chars, subject->length, from, &match, &found)) {
      csPopTempRoot();
      return false;
    }
    if (!found || from > subject->length) break;

    if (match.groups[0].end == match.groups[0].start) {
      /* A zero-width separator splits between every character. */
      if (match.groups[0].start >= subject->length) break;
      from = match.groups[0].start + 1;
      if (match.groups[0].start == pieceStart && pieceStart == 0) continue;
    }

    ObjString *piece = csStringCopy(subject->chars + pieceStart,
                                    match.groups[0].start - pieceStart);
    csPushTempRoot((Obj *)piece);
    csValueArrayWrite(&parts->elements, OBJ_VAL(piece));
    csPopTempRoot();

    pieceStart = match.groups[0].end;
    if (match.groups[0].end > from) from = match.groups[0].end;
  }

  ObjString *tail =
      csStringCopy(subject->chars + pieceStart, subject->length - pieceStart);
  csPushTempRoot((Obj *)tail);
  csValueArrayWrite(&parts->elements, OBJ_VAL(tail));
  csPopTempRoot();

  if (regex->global) regex->lastIndex = 0;
  csPopTempRoot();
  *result = OBJ_VAL(parts);
  return true;
}
