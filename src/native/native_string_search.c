/* native_string_search.c — the string methods that take a pattern.
 *
 * `match`, `search`, `replace` and `split`. Each takes either a plain string
 * or a regex and has to behave the same way for both, which is the reason they
 * are one group: a `split` on a string and a `split` on a regex are different
 * searches with the same contract, and the regex side knows the rules about
 * groups and the `g` flag that the string side has no use for.
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#include "native/native_internal.h"

/* Replaces the first match, or every match when `all` is set. A pattern that
 * is a regex hands off to the regex side, which knows the rules about groups
 * and the `g` flag. */
static bool stringReplaceImpl(Value receiver, int argCount, Value *args, Value *result,
                              bool all, const char *method) {
  if (argCount > 0 && IS_REGEX(args[0])) {
    return csRegexStringReplace(receiver, argCount, args, result, all);
  }

  ObjString *needle;
  if (!csNativeStringArg(argCount, args, 0, method, &needle)) return false;

  /* A function replacer is called once per hit with the match, where it
   * started and the whole subject — the same three arguments the pattern form
   * passes, minus the captures a plain string has none of. */
  bool byFunction = argCount > 1 && csValueIsCallable(args[1]);
  ObjString *replacement = NULL;
  if (!byFunction && !csNativeStringArg(argCount, args, 1, method, &replacement)) return false;

  ObjString *string = AS_STRING(receiver);

  size_t capacity = (size_t)string->length + 16;
  char *buffer = (char *)malloc(capacity);
  if (buffer == NULL) return csNativeFinishString(NULL, 0, result);
  size_t length = 0;

  int cursor = 0;
  bool replaced = false;
  while (cursor <= string->length) {
    int hit = (!replaced || all) ? csNativeFindFrom(string, needle, cursor) : -1;
    /* An empty needle would match forever; stop after the first. */
    if (hit < 0 || (needle->length == 0 && replaced)) break;

    /* The callee is user code and may allocate, so it runs before the buffer
     * is sized — its result is what has to fit. */
    ObjString *piece = replacement;
    if (byFunction) {
      Value argv[3];
      ObjString *matched = csStringCopy(string->chars + hit, needle->length);
      csPushTempRoot((Obj *)matched);
      argv[0] = OBJ_VAL(matched);
      argv[1] = NUMBER_VAL(hit);
      argv[2] = OBJ_VAL(string);

      Value produced;
      bool ok = csVMCallAdapted(args[1], argv, 3, &produced);
      csPopTempRoot();
      if (!ok) {
        free(buffer);
        return false;
      }

      if (IS_STRING(produced)) {
        piece = AS_STRING(produced);
      } else {
        size_t textLength = 0;
        char *text = csValueToCString(produced, &textLength);
        if (text == NULL) {
          free(buffer);
          return csNativeFinishString(NULL, 0, result);
        }
        piece = csStringCopy(text, (int)textLength);
        free(text);
      }
    }

    size_t needed = length + (size_t)(hit - cursor) + (size_t)piece->length + 1;
    if (needed > capacity) {
      while (capacity < needed) capacity *= 2;
      char *grown = (char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(buffer);
        return csNativeFinishString(NULL, 0, result);
      }
      buffer = grown;
    }

    memcpy(buffer + length, string->chars + cursor, (size_t)(hit - cursor));
    length += (size_t)(hit - cursor);
    memcpy(buffer + length, piece->chars, (size_t)piece->length);
    length += (size_t)piece->length;

    cursor = hit + (needle->length > 0 ? needle->length : 1);
    replaced = true;
    if (!all) break;
  }

  if (cursor < string->length) {
    size_t remaining = (size_t)(string->length - cursor);
    size_t needed = length + remaining + 1;
    if (needed > capacity) {
      while (capacity < needed) capacity *= 2;
      char *grown = (char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(buffer);
        return csNativeFinishString(NULL, 0, result);
      }
      buffer = grown;
    }
    memcpy(buffer + length, string->chars + cursor, remaining);
    length += remaining;
  }
  buffer[length] = '\0';
  return csNativeFinishString(buffer, (int)length, result);
}

/* Both take a pattern and nothing else, so they are thin. */
static bool stringMatch(Value receiver, int argCount, Value *args, Value *result) {
  if (argCount < 1 || !IS_REGEX(args[0])) {
    csVMRuntimeError("match expects a regular expression");
    return false;
  }
  return csRegexStringMatch(receiver, argCount, args, result);
}

static bool stringSearch(Value receiver, int argCount, Value *args, Value *result) {
  if (argCount < 1 || !IS_REGEX(args[0])) {
    csVMRuntimeError("search expects a regular expression");
    return false;
  }
  return csRegexStringSearch(receiver, argCount, args, result);
}

static bool stringReplace(Value receiver, int argCount, Value *args, Value *result) {
  return stringReplaceImpl(receiver, argCount, args, result, false, "replace");
}
static bool stringReplaceAll(Value receiver, int argCount, Value *args, Value *result) {
  return stringReplaceImpl(receiver, argCount, args, result, true, "replaceAll");
}

static bool stringSplit(Value receiver, int argCount, Value *args, Value *result) {
  if (argCount > 0 && IS_REGEX(args[0])) {
    return csRegexStringSplit(receiver, argCount, args, result);
  }

  ObjString *string = AS_STRING(receiver);

  ObjArray *pieces = csArrayNew();
  csPushTempRoot((Obj *)pieces);

  /* With no separator the whole string is the single piece. */
  if (argCount < 1 || IS_UNDEFINED(args[0])) {
    csValueArrayWrite(&pieces->elements, receiver);
    csPopTempRoot();
    *result = OBJ_VAL(pieces);
    return true;
  }

  if (!IS_STRING(args[0])) {
    csPopTempRoot();
    csVMRuntimeError("split expects a string, got %s", csValueTypeName(args[0]));
    return false;
  }
  ObjString *separator = AS_STRING(args[0]);

  /* An empty separator splits into single characters. */
  if (separator->length == 0) {
    for (int i = 0; i < string->length; i++) {
      ObjString *piece = csStringCopy(string->chars + i, 1);
      csPushTempRoot((Obj *)piece);
      csValueArrayWrite(&pieces->elements, OBJ_VAL(piece));
      csPopTempRoot();
    }
    csPopTempRoot();
    *result = OBJ_VAL(pieces);
    return true;
  }

  int cursor = 0;
  for (;;) {
    int hit = csNativeFindFrom(string, separator, cursor);
    int end = hit < 0 ? string->length : hit;

    ObjString *piece = csStringCopy(string->chars + cursor, end - cursor);
    csPushTempRoot((Obj *)piece);
    csValueArrayWrite(&pieces->elements, OBJ_VAL(piece));
    csPopTempRoot();

    if (hit < 0) break;
    cursor = hit + separator->length;
  }

  csPopTempRoot();
  *result = OBJ_VAL(pieces);
  return true;
}

void csNativeInstallStringSearch(void) {
  csNativeDefineStringMethod("match", stringMatch, -1);
  csNativeDefineStringMethod("search", stringSearch, -1);
  csNativeDefineStringMethod("replace", stringReplace, -1);
  csNativeDefineStringMethod("replaceAll", stringReplaceAll, -1);
  csNativeDefineStringMethod("split", stringSplit, -1);
}
