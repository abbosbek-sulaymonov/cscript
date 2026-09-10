/* native_string.c — a string's methods that read or reshape it.
 *
 * Strings are immutable and interned, so every one of these builds a new one
 * rather than editing in place — and indexes are by byte, which is correct for
 * ASCII and is the documented limit. The methods that take a *pattern* are in
 * native_string_search.c, because a pattern may be a regex and that pulls the
 * whole engine in with it.
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

static int clampIndex(double raw, int length) {
  int index = (int)raw;
  if (index < 0) index += length;
  if (index < 0) return 0;
  if (index > length) return length;
  return index;
}

bool csNativeStringArg(int argCount, Value *args, int position, const char *method,
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
bool csNativeFinishString(char *buffer, int length, Value *result) {
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
  if (buffer == NULL) return csNativeFinishString(NULL, 0, result);

  for (int i = 0; i < string->length; i++) {
    unsigned char c = (unsigned char)string->chars[i];
    buffer[i] = (char)(upper ? toupper(c) : tolower(c));
  }
  buffer[string->length] = '\0';
  return csNativeFinishString(buffer, string->length, result);
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

/* Like charAt, except that a negative index counts from the end and a missing
 * one answers undefined rather than an empty string. */
static bool stringAt(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *string = AS_STRING(receiver);
  double raw = argCount > 0 && IS_NUMBER(args[0]) ? AS_NUMBER(args[0]) : 0;

  int index = (int)raw;
  if (index < 0) index += string->length;
  if (index < 0 || index >= string->length) {
    *result = UNDEFINED_VAL;
    return true;
  }
  *result = OBJ_VAL(csStringCopy(string->chars + index, 1));
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
int csNativeFindFrom(ObjString *haystack, ObjString *needle, int from) {
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
  if (!csNativeStringArg(argCount, args, 0, "indexOf", &needle)) return false;

  double from;
  if (!numberArg(argCount, args, 1, 0, "indexOf", &from)) return false;

  *result = NUMBER_VAL(csNativeFindFrom(AS_STRING(receiver), needle, (int)from));
  return true;
}

static bool stringLastIndexOf(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!csNativeStringArg(argCount, args, 0, "lastIndexOf", &needle)) return false;
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
  if (!csNativeStringArg(argCount, args, 0, "includes", &needle)) return false;
  *result = BOOL_VAL(csNativeFindFrom(AS_STRING(receiver), needle, 0) >= 0);
  return true;
}

static bool stringStartsWith(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!csNativeStringArg(argCount, args, 0, "startsWith", &needle)) return false;
  ObjString *string = AS_STRING(receiver);

  *result = BOOL_VAL(needle->length <= string->length &&
                     memcmp(string->chars, needle->chars, (size_t)needle->length) == 0);
  return true;
}

static bool stringEndsWith(Value receiver, int argCount, Value *args, Value *result) {
  ObjString *needle;
  if (!csNativeStringArg(argCount, args, 0, "endsWith", &needle)) return false;
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
  if (buffer == NULL) return csNativeFinishString(NULL, 0, result);

  for (int i = 0; i < times; i++) {
    memcpy(buffer + (size_t)i * (size_t)string->length, string->chars,
           (size_t)string->length);
  }
  buffer[length] = '\0';
  return csNativeFinishString(buffer, (int)length, result);
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
  if (buffer == NULL) return csNativeFinishString(NULL, 0, result);

  int offset = atStart ? 0 : string->length;
  if (!atStart) memcpy(buffer, string->chars, (size_t)string->length);
  for (int i = 0; i < padLength; i++) buffer[offset + i] = filler[i % fillerLength];
  if (atStart) memcpy(buffer + padLength, string->chars, (size_t)string->length);

  buffer[target] = '\0';
  return csNativeFinishString(buffer, target, result);
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
  if (buffer == NULL) return csNativeFinishString(NULL, 0, result);

  size_t offset = (size_t)string->length;
  memcpy(buffer, string->chars, offset);
  for (int i = 0; i < argCount; i++) {
    size_t pieceLength = 0;
    char *piece = csValueToCString(args[i], &pieceLength);
    if (piece == NULL) {
      free(buffer);
      return csNativeFinishString(NULL, 0, result);
    }
    memcpy(buffer + offset, piece, pieceLength);
    offset += pieceLength;
    free(piece);
  }
  buffer[offset] = '\0';
  return csNativeFinishString(buffer, (int)offset, result);
}

/* ---------------- installation ---------------- */

void csNativeDefineStringMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.stringMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csStringMethodsInstall(void) {
  csNativeDefineStringMethod("toUpperCase", stringToUpper, 0);
  csNativeDefineStringMethod("toLowerCase", stringToLower, 0);
  csNativeDefineStringMethod("trim", stringTrim, 0);
  csNativeDefineStringMethod("trimStart", stringTrimStart, 0);
  csNativeDefineStringMethod("trimEnd", stringTrimEnd, 0);
  csNativeDefineStringMethod("slice", stringSlice, -1);
  csNativeDefineStringMethod("substring", stringSubstring, -1);
  csNativeDefineStringMethod("charAt", stringCharAt, -1);
  csNativeDefineStringMethod("at", stringAt, -1);
  csNativeDefineStringMethod("charCodeAt", stringCharCodeAt, -1);
  csNativeDefineStringMethod("indexOf", stringIndexOf, -1);
  csNativeDefineStringMethod("lastIndexOf", stringLastIndexOf, -1);
  csNativeDefineStringMethod("includes", stringIncludes, -1);
  csNativeDefineStringMethod("startsWith", stringStartsWith, -1);
  csNativeDefineStringMethod("endsWith", stringEndsWith, -1);
  csNativeDefineStringMethod("repeat", stringRepeat, -1);
  csNativeDefineStringMethod("padStart", stringPadStart, -1);
  csNativeDefineStringMethod("padEnd", stringPadEnd, -1);
  csNativeDefineStringMethod("concat", stringConcat, -1);

  csNativeInstallStringSearch();
}
