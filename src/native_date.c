/* native_date.c — one instant, and the ways of writing it down.
 *
 * A Date is a double: the number of milliseconds since 1970, which is exactly
 * what JavaScript says one is. Every getter here takes that number apart, and
 * every constructor puts one together — there is no other state.
 *
 * The calendar arithmetic is done here rather than through `mktime`, because
 * `mktime` normalises against the local timezone and there is no portable way
 * to ask it not to. Days since the epoch is a short, exact calculation, and
 * being exact matters more than being brief for something a program will
 * compare and subtract.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* The broken-down form, in whichever zone the caller asked for. */
typedef struct {
  int year, month, day, weekday;
  int hour, minute, second, millisecond;
} Parts;

static bool isLeap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const int MONTH_DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int daysInMonth(int year, int month) {
  return month == 1 && isLeap(year) ? 29 : MONTH_DAYS[month];
}

/* Days from 1970-01-01 to the first of `year`. Counted rather than looped over
 * where the year is far away, so a date in the year 3000 costs the same as one
 * next week. */
static long long daysBeforeYear(int year) {
  long long y = year - 1;
  long long leaps = y / 4 - y / 100 + y / 400;
  long long before = y * 365 + leaps;
  /* The same count for 1969, subtracted, gives days since the epoch. */
  return before - (1969LL * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400);
}

static long long daysFromCivil(int year, int month, int day) {
  long long days = daysBeforeYear(year);
  for (int m = 0; m < month; m++) days += daysInMonth(year, m);
  return days + day - 1;
}

/* The offset local time is ahead of UTC at `ms`, in milliseconds. Asked of the
 * C library rather than computed, because it is the only thing here that
 * depends on where the program is running. */
static double localOffsetMs(double ms) {
  time_t seconds = (time_t)floor(ms / 1000.0);
  struct tm local;
  struct tm utc;
  if (localtime_r(&seconds, &local) == NULL || gmtime_r(&seconds, &utc) == NULL) {
    return 0;
  }

  long long localDays = daysFromCivil(local.tm_year + 1900, local.tm_mon, local.tm_mday);
  long long utcDays = daysFromCivil(utc.tm_year + 1900, utc.tm_mon, utc.tm_mday);
  long long localSeconds =
      localDays * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  long long utcSeconds =
      utcDays * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
  return (double)(localSeconds - utcSeconds) * 1000.0;
}

static bool breakDown(double ms, bool utc, Parts *out) {
  if (ms != ms) return false;
  if (!utc) ms += localOffsetMs(ms);

  double dayMs = floor(ms / 86400000.0);
  double rest = ms - dayMs * 86400000.0;

  long long days = (long long)dayMs;
  out->weekday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */

  int year = 1970;
  while (days < 0) {
    year--;
    days += isLeap(year) ? 366 : 365;
  }
  for (;;) {
    int length = isLeap(year) ? 366 : 365;
    if (days < length) break;
    days -= length;
    year++;
  }

  int month = 0;
  while (days >= daysInMonth(year, month)) {
    days -= daysInMonth(year, month);
    month++;
  }

  out->year = year;
  out->month = month;
  out->day = (int)days + 1;
  out->hour = (int)floor(rest / 3600000.0);
  rest -= out->hour * 3600000.0;
  out->minute = (int)floor(rest / 60000.0);
  rest -= out->minute * 60000.0;
  out->second = (int)floor(rest / 1000.0);
  out->millisecond = (int)(rest - out->second * 1000.0);
  return true;
}

bool csDateToISO(double ms, char *out, size_t size) {
  Parts parts;
  if (!breakDown(ms, true, &parts)) return false;
  snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.year,
           parts.month + 1, parts.day, parts.hour, parts.minute, parts.second,
           parts.millisecond);
  return true;
}

/* `2024-01-31T12:00:00.000Z` and the shorter forms of it. Anything else is a
 * date that could not be parsed, which is NaN rather than an error — the same
 * answer JavaScript gives. */
static double parseISO(const char *text, int length) {
  int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, milli = 0;
  char buffer[64];
  if (length <= 0 || length >= (int)sizeof buffer) return NAN;
  memcpy(buffer, text, (size_t)length);
  buffer[length] = '\0';

  /* Longest form first, because a shorter pattern matches a prefix of a longer
   * string and would leave the rest unread. `%n` is only assigned when the
   * conversion before it succeeded, so `read` is checked rather than trusted. */
  int read = -1;
  if (sscanf(buffer, "%d-%d-%dT%d:%d:%d.%d%n", &year, &month, &day, &hour, &minute,
             &second, &milli, &read) < 7 || read < 0) {
    read = -1;
    milli = 0;
    if (sscanf(buffer, "%d-%d-%dT%d:%d:%d%n", &year, &month, &day, &hour, &minute,
               &second, &read) < 6 || read < 0) {
      read = -1;
      hour = minute = second = 0;
      if (sscanf(buffer, "%d-%d-%d%n", &year, &month, &day, &read) < 3 || read < 0) {
        return NAN;
      }
    }
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) return NAN;
  if (hour > 24 || minute > 59 || second > 60) return NAN;

  double ms = (double)daysFromCivil(year, month - 1, day) * 86400000.0 +
              hour * 3600000.0 + minute * 60000.0 + second * 1000.0 + milli;

  /* A trailing `Z`, or nothing, means UTC. Anything else is a zone this does
   * not read, and saying so beats being quietly an hour out. */
  const char *tail = buffer + read;
  while (*tail == ' ') tail++;
  if (*tail == 'Z' || *tail == '\0') return ms;
  return NAN;
}

double csDateNowMs(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static bool numberAt(int argCount, Value *args, int index, double fallback,
                     double *out) {
  if (argCount <= index) {
    *out = fallback;
    return true;
  }
  if (!IS_NUMBER(args[index])) return false;
  *out = AS_NUMBER(args[index]);
  return true;
}

static bool dateConstruct(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  double ms;

  if (argCount == 0) {
    ms = csDateNowMs();
  } else if (argCount == 1 && IS_STRING(args[0])) {
    ms = parseISO(AS_STRING(args[0])->chars, AS_STRING(args[0])->length);
  } else if (argCount == 1 && IS_NUMBER(args[0])) {
    ms = AS_NUMBER(args[0]);
  } else if (argCount == 1 && IS_DATE(args[0])) {
    ms = AS_DATE(args[0])->ms;
  } else {
    /* `new Date(y, m, d, …)` reads its components as local time, which is what
     * makes it the one constructor whose answer depends on where it runs. */
    double year, month, day, hour, minute, second, milli;
    if (!numberAt(argCount, args, 0, 1970, &year) ||
        !numberAt(argCount, args, 1, 0, &month) ||
        !numberAt(argCount, args, 2, 1, &day) ||
        !numberAt(argCount, args, 3, 0, &hour) ||
        !numberAt(argCount, args, 4, 0, &minute) ||
        !numberAt(argCount, args, 5, 0, &second) ||
        !numberAt(argCount, args, 6, 0, &milli)) {
      csVMRuntimeError("Date expects numbers for its components");
      return false;
    }

    double utc = (double)daysFromCivil((int)year, (int)month, (int)day) * 86400000.0 +
                 hour * 3600000.0 + minute * 60000.0 + second * 1000.0 + milli;
    /* The offset is read at the instant itself, so a date on the far side of a
     * daylight-saving change gets that side's offset. */
    ms = utc - localOffsetMs(utc);
  }

  *result = OBJ_VAL(csDateNew(ms));
  return true;
}

static bool dateNow(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  (void)argCount;
  (void)args;
  *result = NUMBER_VAL(csDateNowMs());
  return true;
}

static bool requireDate(Value receiver, const char *method) {
  if (IS_DATE(receiver)) return true;
  csVMRuntimeError("'%s' needs a Date, got %s", method, csValueTypeName(receiver));
  return false;
}

/* Every getter is the same shape: break the instant down, hand back one piece.
 * `field` says which, and `utc` which zone. */
typedef enum {
  FIELD_YEAR, FIELD_MONTH, FIELD_DAY, FIELD_WEEKDAY,
  FIELD_HOUR, FIELD_MINUTE, FIELD_SECOND, FIELD_MILLISECOND,
} DateField;

static bool getPart(Value receiver, const char *method, DateField field, bool utc,
                    Value *result) {
  if (!requireDate(receiver, method)) return false;

  Parts parts;
  if (!breakDown(AS_DATE(receiver)->ms, utc, &parts)) {
    *result = NUMBER_VAL(NAN);
    return true;
  }

  int value = 0;
  switch (field) {
    case FIELD_YEAR:        value = parts.year; break;
    case FIELD_MONTH:       value = parts.month; break;
    case FIELD_DAY:         value = parts.day; break;
    case FIELD_WEEKDAY:     value = parts.weekday; break;
    case FIELD_HOUR:        value = parts.hour; break;
    case FIELD_MINUTE:      value = parts.minute; break;
    case FIELD_SECOND:      value = parts.second; break;
    case FIELD_MILLISECOND: value = parts.millisecond; break;
  }
  *result = NUMBER_VAL(value);
  return true;
}

#define DATE_GETTER(fn, method, field, utc)                                    \
  static bool fn(Value receiver, int argCount, Value *args, Value *result) {   \
    (void)argCount;                                                            \
    (void)args;                                                                \
    return getPart(receiver, method, field, utc, result);                      \
  }

DATE_GETTER(dateFullYear, "getFullYear", FIELD_YEAR, false)
DATE_GETTER(dateMonth, "getMonth", FIELD_MONTH, false)
DATE_GETTER(dateDate, "getDate", FIELD_DAY, false)
DATE_GETTER(dateDay, "getDay", FIELD_WEEKDAY, false)
DATE_GETTER(dateHours, "getHours", FIELD_HOUR, false)
DATE_GETTER(dateMinutes, "getMinutes", FIELD_MINUTE, false)
DATE_GETTER(dateSeconds, "getSeconds", FIELD_SECOND, false)
DATE_GETTER(dateMilliseconds, "getMilliseconds", FIELD_MILLISECOND, false)
DATE_GETTER(dateUTCFullYear, "getUTCFullYear", FIELD_YEAR, true)
DATE_GETTER(dateUTCMonth, "getUTCMonth", FIELD_MONTH, true)
DATE_GETTER(dateUTCDate, "getUTCDate", FIELD_DAY, true)
DATE_GETTER(dateUTCDay, "getUTCDay", FIELD_WEEKDAY, true)
DATE_GETTER(dateUTCHours, "getUTCHours", FIELD_HOUR, true)
DATE_GETTER(dateUTCMinutes, "getUTCMinutes", FIELD_MINUTE, true)
DATE_GETTER(dateUTCSeconds, "getUTCSeconds", FIELD_SECOND, true)
DATE_GETTER(dateUTCMilliseconds, "getUTCMilliseconds", FIELD_MILLISECOND, true)

static bool dateGetTime(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  if (!requireDate(receiver, "getTime")) return false;
  *result = NUMBER_VAL(AS_DATE(receiver)->ms);
  return true;
}

static bool dateTimezoneOffset(Value receiver, int argCount, Value *args,
                               Value *result) {
  (void)argCount;
  (void)args;
  if (!requireDate(receiver, "getTimezoneOffset")) return false;
  /* Minutes *behind* UTC, which is the sign JavaScript uses. */
  *result = NUMBER_VAL(-localOffsetMs(AS_DATE(receiver)->ms) / 60000.0);
  return true;
}

static bool dateToISOString(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  if (!requireDate(receiver, "toISOString")) return false;

  char text[64];
  if (!csDateToISO(AS_DATE(receiver)->ms, text, sizeof text)) {
    csVMRuntimeError("this date cannot be written down: it is not a real time");
    return false;
  }
  *result = OBJ_VAL(csStringCopy(text, (int)strlen(text)));
  return true;
}

static bool dateToJSON(Value receiver, int argCount, Value *args, Value *result) {
  if (!requireDate(receiver, "toJSON")) return false;
  if (AS_DATE(receiver)->ms != AS_DATE(receiver)->ms) {
    *result = NULL_VAL; /* JSON has no way to write an invalid date */
    return true;
  }
  return dateToISOString(receiver, argCount, args, result);
}

static bool dateToString(Value receiver, int argCount, Value *args, Value *result) {
  (void)argCount;
  (void)args;
  if (!requireDate(receiver, "toString")) return false;

  char text[64];
  if (!csDateToISO(AS_DATE(receiver)->ms, text, sizeof text)) {
    *result = OBJ_VAL(csStringCopy("Invalid Date", 12));
    return true;
  }
  *result = OBJ_VAL(csStringCopy(text, (int)strlen(text)));
  return true;
}

static void defineDateMethod(const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(&vm.dateMethods, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

NativeFn csDateConstructorFn(void) { return dateConstruct; }

void csDateInstallStatics(ObjObject *statics) {
  ObjNative *now = csNativeNew(dateNow, "now", 0);
  csPushTempRoot((Obj *)now);
  csObjectSetProperty(statics, "now", OBJ_VAL(now));
  csPopTempRoot();
}

void csDateMethodsInstall(void) {
  defineDateMethod("getTime", dateGetTime, 0);
  defineDateMethod("valueOf", dateGetTime, 0);
  defineDateMethod("getTimezoneOffset", dateTimezoneOffset, 0);
  defineDateMethod("toISOString", dateToISOString, 0);
  defineDateMethod("toJSON", dateToJSON, -1);
  defineDateMethod("toString", dateToString, 0);

  defineDateMethod("getFullYear", dateFullYear, 0);
  defineDateMethod("getMonth", dateMonth, 0);
  defineDateMethod("getDate", dateDate, 0);
  defineDateMethod("getDay", dateDay, 0);
  defineDateMethod("getHours", dateHours, 0);
  defineDateMethod("getMinutes", dateMinutes, 0);
  defineDateMethod("getSeconds", dateSeconds, 0);
  defineDateMethod("getMilliseconds", dateMilliseconds, 0);

  defineDateMethod("getUTCFullYear", dateUTCFullYear, 0);
  defineDateMethod("getUTCMonth", dateUTCMonth, 0);
  defineDateMethod("getUTCDate", dateUTCDate, 0);
  defineDateMethod("getUTCDay", dateUTCDay, 0);
  defineDateMethod("getUTCHours", dateUTCHours, 0);
  defineDateMethod("getUTCMinutes", dateUTCMinutes, 0);
  defineDateMethod("getUTCSeconds", dateUTCSeconds, 0);
  defineDateMethod("getUTCMilliseconds", dateUTCMilliseconds, 0);
}
