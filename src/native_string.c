/* native_string.c — the built-in string methods.
 *
 * Strings are immutable and interned, so every method here produces a new
 * string through csStringCopy rather than editing in place. Two consequences:
 * a method that changes nothing can return its receiver unchanged, and any
 * intermediate buffer is plain malloc memory the collector never sees.
 *
 * Indexing is by byte. That is correct for ASCII and wrong for multi-byte
 * UTF-8, which is stated in docs/GRAMMAR.md rather than papered over.
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* Clamps a possibly-negative index the way JavaScript's slice does. */
static int clampIndex(double raw, int length) {
  int index = (int)raw;
  if (index < 0) index += length;
  if (index < 0) return 0;
  if (index > length) return length;
  return index;
}

static bool stringArg(int argCount, Value *args, int position, const char *method,
                      ObjString **out) {
  if (argCount <= position || !IS_STRING(args[position])) {
    csVMRuntimeError("%s expects a string, got %s", method,
                     argCount > position ? csValueTypeName(args[position])
                                         : "no argument");
    return false;
  }
  *out = AS_STRING(args[position]);
  return true;
}

static bool numberArg(int argCount, Value *args, int position, double fallback,
                      const char *method, double *out) {
  if (argCount <= position || IS_UNDEFINED(args[position])) {
    *out = fallback;
    return true;
  }
  if (!IS_NUMBER(args[position])) {
    csVMRuntimeError("%s expects a number, got %s", method,
                     csValueTypeName(args[position]));
    return false;
  }
  *out = AS_NUMBER(args[position]);
  return true;
}

/* Builds a result string from a malloc'd buffer and releases the buffer. */
static bool finishString(char *buffer, int length, Value *result) {
  if (buffer == NULL) {
    csVMRuntimeError("out of memory building a string");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(buffer, length));
  free(buffer);
  return true;
}

static bool stringCase(Value receiver, Value *result, bool upper) {
  ObjString *string = AS_STRING(receiver);
  char *buffer = (char *)malloc((size_t)string->length + 1);
  if (buffer == NULL) return finishString(NULL, 0, result);

  for (int i = 0; i < string->length; i++) {
    unsigned char c = (unsigned char)string->chars[i];
    buffer[i] = (char)(upper ? toupper(c) : tolower(c));
  }
  buffer[string->length] = '\0';
  return finishString(buffer, string->length, result);
}

static bool stringToUpper(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  return stringCase(receiver, result, true);
}

static bool stringToLower(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  return stringCase(receiver, result, false);
}

static bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool stringTrimRange(Value receiver, Value *result, bool start, bool end) {
  ObjString *string = AS_STRING(receiver);
  int from = 0;
  int to = string->length;

  if (start) {
    while (from < to && isSpace(string->chars[from])) from++;
  }
  if (end) {
    while (to > from && isSpace(string->chars[to - 1])) to--;
  }

  *result = OBJ_VAL(csStringCopy(string->chars + from, to - from));
  return true;
}

static bool stringTrim(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  return stringTrimRange(receiver, result, true, true);
}
static bool stringTrimStart(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  return stringTrimRange(receiver, result, true, false);
}
static bool stringTrimEnd(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  return stringTrimRange(receiver, result, false, true);
}

static bool stringSlice(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);

  double rawStart, rawEnd;
  if (!numberArg(argCount, args, 0, 0, "slice", &rawStart)) return false;
  if (!numberArg(argCount, args, 1, string->length, "slice", &rawEnd)) return false;

  int start = clampIndex(rawStart, string->length);
  int end = clampIndex(rawEnd, string->length);
  if (end < start) end = start;

  *result = OBJ_VAL(csStringCopy(string->chars + start, end - start));
  return true;
}

/* substring differs from slice: negatives clamp to 0 and the bounds swap if
 * they are the wrong way round. */
static bool stringSubstring(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);

  double rawStart, rawEnd;
  if (!numberArg(argCount, args, 0, 0, "substring", &rawStart)) return false;
  if (!numberArg(argCount, args, 1, string->length, "substring", &rawEnd)) return false;

  int start = (int)rawStart < 0 ? 0 : (int)rawStart;
  int end = (int)rawEnd < 0 ? 0 : (int)rawEnd;
  if (start > string->length) start = string->length;
  if (end > string->length) end = string->length;
  if (start > end) {
    int swap = start;
    start = end;
    end = swap;
  }

  *result = OBJ_VAL(csStringCopy(string->chars + start, end - start));
  return true;
}

static bool stringCharAt(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);
  double raw;
  if (!numberArg(argCount, args, 0, 0, "charAt", &raw)) return false;

  int index = (int)raw;
  if (index < 0 || index >= string->length) {
    *result = OBJ_VAL(csStringCopy("", 0));
    return true;
  }
  *result = OBJ_VAL(csStringCopy(string->chars + index, 1));
  return true;
}

static bool stringCharCodeAt(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);
  double raw;
  if (!numberArg(argCount, args, 0, 0, "charCodeAt", &raw)) return false;

  int index = (int)raw;
  if (index < 0 || index >= string->length) {
    *result = NUMBER_VAL(NAN); /* out of range is NaN, as in JavaScript */
    return true;
  }
  *result = NUMBER_VAL((unsigned char)string->chars[index]);
  return true;
}

/* Returns the byte offset of `needle` in `haystack` at or after `from`, or -1. */
static int findFrom(ObjString *haystack, ObjString *needle, int from) {
  if (needle->length == 0) return from <= haystack->length ? from : haystack->length;
  if (from < 0) from = 0;

  for (int i = from; i + needle->length <= haystack->length; i++) {
    if (memcmp(haystack->chars + i, needle->chars, (size_t)needle->length) == 0) {
      return i;
    }
  }
  return -1;
}

static bool stringIndexOf(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!stringArg(argCount, args, 0, "indexOf", &needle)) return false;

  double from;
  if (!numberArg(argCount, args, 1, 0, "indexOf", &from)) return false;

  *result = NUMBER_VAL(findFrom(AS_STRING(receiver), needle, (int)from));
  return true;
}

static bool stringLastIndexOf(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!stringArg(argCount, args, 0, "lastIndexOf", &needle)) return false;
  ObjString *string = AS_STRING(receiver);

  for (int i = string->length - needle->length; i >= 0; i--) {
    if (memcmp(string->chars + i, needle->chars, (size_t)needle->length) == 0) {
      *result = NUMBER_VAL(i);
      return true;
    }
  }
  *result = NUMBER_VAL(-1);
  return true;
}

static bool stringIncludes(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!stringArg(argCount, args, 0, "includes", &needle)) return false;
  *result = BOOL_VAL(findFrom(AS_STRING(receiver), needle, 0) >= 0);
  return true;
}

static bool stringStartsWith(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!stringArg(argCount, args, 0, "startsWith", &needle)) return false;
  ObjString *string = AS_STRING(receiver);

  *result = BOOL_VAL(needle->length <= string->length &&
                     memcmp(string->chars, needle->chars, (size_t)needle->length) == 0);
  return true;
}

static bool stringEndsWith(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!stringArg(argCount, args, 0, "endsWith", &needle)) return false;
  ObjString *string = AS_STRING(receiver);

  int offset = string->length - needle->length;
  *result = BOOL_VAL(offset >= 0 &&
                     memcmp(string->chars + offset, needle->chars,
                            (size_t)needle->length) == 0);
  return true;
}

static bool stringRepeat(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);
  double raw;
  if (!numberArg(argCount, args, 0, 0, "repeat", &raw)) return false;

  int times = (int)raw;
  if (times < 0) {
    csVMRuntimeError("repeat count must not be negative");
    return false;
  }

  size_t length = (size_t)string->length * (size_t)times;
  char *buffer = (char *)malloc(length + 1);
  if (buffer == NULL) return finishString(NULL, 0, result);

  for (int i = 0; i < times; i++) {
    memcpy(buffer + (size_t)i * (size_t)string->length, string->chars,
           (size_t)string->length);
  }
  buffer[length] = '\0';
  return finishString(buffer, (int)length, result);
}

/* Replaces the first match, or every match when `all` is set. */
/* A pattern where a string was expected hands off to the regex side, which
 * knows the rules about groups and the `g` flag. */
static bool stringReplaceImpl(Value receiver, int argCount, Value *args, Value *result,
                              bool all, const char *method) {
  if (argCount > 0 && IS_REGEX(args[0])) {
    return csRegexStringReplace(receiver, argCount, args, result, all);
  }

  ObjString *needle;
  if (!stringArg(argCount, args, 0, method, &needle)) return false;

  /* A function replacer is called once per hit with the match, where it
   * started and the whole subject — the same three arguments the pattern form
   * passes, minus the captures a plain string has none of. */
  bool byFunction = argCount > 1 && csValueIsCallable(args[1]);
  ObjString *replacement = NULL;
  if (!byFunction && !stringArg(argCount, args, 1, method, &replacement)) return false;

  ObjString *string = AS_STRING(receiver);

  size_t capacity = (size_t)string->length + 16;
  char *buffer = (char *)malloc(capacity);
  if (buffer == NULL) return finishString(NULL, 0, result);
  size_t length = 0;

  int cursor = 0;
  bool replaced = false;
  while (cursor <= string->length) {
    int hit = (!replaced || all) ? findFrom(string, needle, cursor) : -1;
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
          return finishString(NULL, 0, result);
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
        return finishString(NULL, 0, result);
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
        return finishString(NULL, 0, result);
      }
      buffer = grown;
    }
    memcpy(buffer + length, string->chars + cursor, remaining);
    length += remaining;
  }
  buffer[length] = '\0';
  return finishString(buffer, (int)length, result);
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
    int hit = findFrom(string, separator, cursor);
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

static bool stringPad(Value receiver, int argCount, Value *args, Value *result,
                      bool atStart, const char *method) {
  ObjString *string = AS_STRING(receiver);

  double raw;
  if (!numberArg(argCount, args, 0, 0, method, &raw)) return false;
  int target = (int)raw;
  if (target <= string->length) {
    *result = receiver;
    return true;
  }

  const char *filler = " ";
  int fillerLength = 1;
  if (argCount >= 2 && !IS_UNDEFINED(args[1])) {
    if (!IS_STRING(args[1])) {
      csVMRuntimeError("%s expects a string, got %s", method,
                       csValueTypeName(args[1]));
      return false;
    }
    filler = AS_CSTRING(args[1]);
    fillerLength = AS_STRING(args[1])->length;
    if (fillerLength == 0) {
      *result = receiver;
      return true;
    }
  }

  int padLength = target - string->length;
  char *buffer = (char *)malloc((size_t)target + 1);
  if (buffer == NULL) return finishString(NULL, 0, result);

  int offset = atStart ? 0 : string->length;
  if (!atStart) memcpy(buffer, string->chars, (size_t)string->length);
  for (int i = 0; i < padLength; i++) buffer[offset + i] = filler[i % fillerLength];
  if (atStart) memcpy(buffer + padLength, string->chars, (size_t)string->length);

  buffer[target] = '\0';
  return finishString(buffer, target, result);
}

static bool stringPadStart(Value receiver, int argCount, Value *args, Value *result) {
  return stringPad(receiver, argCount, args, result, true, "padStart");
}
static bool stringPadEnd(Value receiver, int argCount, Value *args, Value *result) {
  return stringPad(receiver, argCount, args, result, false, "padEnd");
}

static bool stringConcat(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);

  size_t length = (size_t)string->length;
  for (int i = 0; i < argCount; i++) {
    size_t pieceLength = 0;
    char *piece = csValueToCString(args[i], &pieceLength);
    free(piece);
    length += pieceLength;
  }

  char *buffer = (char *)malloc(length + 1);
  if (buffer == NULL) return finishString(NULL, 0, result);

  size_t offset = (size_t)string->length;
  memcpy(buffer, string->chars, offset);
  for (int i = 0; i < argCount; i++) {
    size_t pieceLength = 0;
    char *piece = csValueToCString(args[i], &pieceLength);
    if (piece == NULL) {
      free(buffer);
      return finishString(NULL, 0, result);
    }
    memcpy(buffer + offset, piece, pieceLength);
    offset += pieceLength;
    free(piece);
  }
  buffer[offset] = '\0';
  return finishString(buffer, (int)offset, result);
}

static void defineStringMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.stringMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csStringMethodsInstall(void) {
  defineStringMethod("toUpperCase", stringToUpper, 0);
  defineStringMethod("toLowerCase", stringToLower, 0);
  defineStringMethod("trim", stringTrim, 0);
  defineStringMethod("trimStart", stringTrimStart, 0);
  defineStringMethod("trimEnd", stringTrimEnd, 0);
  defineStringMethod("slice", stringSlice, -1);
  defineStringMethod("substring", stringSubstring, -1);
  defineStringMethod("charAt", stringCharAt, -1);
  defineStringMethod("charCodeAt", stringCharCodeAt, -1);
  defineStringMethod("indexOf", stringIndexOf, -1);
  defineStringMethod("lastIndexOf", stringLastIndexOf, -1);
  defineStringMethod("includes", stringIncludes, -1);
  defineStringMethod("startsWith", stringStartsWith, -1);
  defineStringMethod("endsWith", stringEndsWith, -1);
  defineStringMethod("repeat", stringRepeat, -1);
  defineStringMethod("match", stringMatch, -1);
  defineStringMethod("search", stringSearch, -1);
  defineStringMethod("replace", stringReplace, -1);
  defineStringMethod("replaceAll", stringReplaceAll, -1);
  defineStringMethod("split", stringSplit, -1);
  defineStringMethod("padStart", stringPadStart, -1);
  defineStringMethod("padEnd", stringPadEnd, -1);
  defineStringMethod("concat", stringConcat, -1);
}
