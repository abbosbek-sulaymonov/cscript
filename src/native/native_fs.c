/* native_fs.c — `fs`, the only way a CScript program reaches a file.
 *
 * Every operation answers an object rather than throwing: `{ ok: true, … }` or
 * `{ ok: false, error: "No such file or directory" }`. That is not a style
 * choice — a runtime error in CScript is not catchable, so a failed read that
 * reported one would take the process down, and reading a file that might not
 * be there is the most ordinary thing a script does. Answering a value is what
 * lets `std:io` turn it into a Result.
 *
 * Synchronous, deliberately. CScript's event loop drives promises and timers,
 * not I/O, so an asynchronous file API here would be a synchronous read
 * wearing a promise — which reads like a guarantee it cannot make.
 *
 * What is deliberately absent: networking, spawning a process, and anything
 * that watches. Each is a project of its own, and a standard library that
 * pretends otherwise is worse than one that says so.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

#include "native/native_internal.h"

/* An answer, built and rooted. Every function here ends in one of these two,
 * so the shape a caller matches on is decided in exactly one place. */
static ObjObject *beginAnswer(bool ok) {
  ObjObject *answer = csObjectNew("Object");
  csPushTempRoot((Obj *)answer);
  csObjectSetProperty(answer, "ok", BOOL_VAL(ok));
  return answer;
}

static bool finishAnswer(ObjObject *answer, Value *result) {
  *result = OBJ_VAL(answer);
  csPopTempRoot();
  return true;
}

/* `strerror` rather than the number: a script cannot do anything with 2, and
 * "No such file or directory" is the same message every other tool prints. */
static bool failure(int code, Value *result) {
  ObjObject *answer = beginAnswer(false);
  const char *text = strerror(code);
  ObjString *message = csStringCopy(text, (int)strlen(text));
  csPushTempRoot((Obj *)message);
  csObjectSetProperty(answer, "error", OBJ_VAL(message));
  csPopTempRoot();
  return finishAnswer(answer, result);
}

/* The one argument all of these take. Answers NULL with the error already
 * reported — a path that is not a string is a mistake in the program rather
 * than a condition to hand back. */
static ObjString *pathArgument(int argCount, Value *args, const char *forWhat) {
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("%s expects a path as its first argument", forWhat);
    return NULL;
  }
  return AS_STRING(args[0]);
}

static bool fsRead(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.read");
  if (path == NULL) return false;

  FILE *file = fopen(path->chars, "rb");
  if (file == NULL) return failure(errno, result);

  /* Sized from the file rather than grown: one allocation, and a short read is
   * then a real error instead of a truncation nothing notices. */
  if (fseek(file, 0, SEEK_END) != 0) {
    int code = errno;
    fclose(file);
    return failure(code, result);
  }
  long size = ftell(file);
  if (size < 0) {
    int code = errno;
    fclose(file);
    return failure(code, result);
  }
  rewind(file);

  char *buffer = (char *)malloc((size_t)size + 1);
  if (buffer == NULL) {
    fclose(file);
    return failure(ENOMEM, result);
  }
  size_t read = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    free(buffer);
    return failure(EIO, result);
  }
  buffer[read] = '\0';

  ObjString *text = csStringCopy(buffer, (int)read);
  free(buffer);
  csPushTempRoot((Obj *)text);
  ObjObject *answer = beginAnswer(true);
  csObjectSetProperty(answer, "text", OBJ_VAL(text));
  bool done = finishAnswer(answer, result);
  csPopTempRoot();
  return done;
}

/* The body of both write and append, which differ only in the mode. */
static bool writeWithMode(int argCount, Value *args, Value *result, const char *mode, const char *forWhat) {
  ObjString *path = pathArgument(argCount, args, forWhat);
  if (path == NULL) return false;
  if (argCount < 2 || !IS_STRING(args[1])) {
    csVMRuntimeError("%s expects the text to write as its second argument", forWhat);
    return false;
  }
  ObjString *text = AS_STRING(args[1]);

  FILE *file = fopen(path->chars, mode);
  if (file == NULL) return failure(errno, result);

  size_t written = text->length > 0 ? fwrite(text->chars, 1, (size_t)text->length, file) : 0;
  /* Closing is what flushes, so its failure is the write's failure — a
   * successful fwrite into a buffer that never reached the disk is not a
   * successful write. */
  bool closed = fclose(file) == 0;
  if (written != (size_t)text->length || !closed) return failure(EIO, result);

  ObjObject *answer = beginAnswer(true);
  csObjectSetProperty(answer, "bytes", NUMBER_VAL((double)written));
  return finishAnswer(answer, result);
}

static bool fsWrite(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return writeWithMode(argCount, args, result, "wb", "fs.write");
}

static bool fsAppend(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return writeWithMode(argCount, args, result, "ab", "fs.append");
}

/* A plain boolean, because "is it there" has no failure mode worth reporting:
 * everything that would make the answer unknowable also makes it no. */
static bool fsExists(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.exists");
  if (path == NULL) return false;
  *result = BOOL_VAL(access(path->chars, F_OK) == 0);
  return true;
}

static bool fsStat(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.stat");
  if (path == NULL) return false;

  struct stat info;
  if (stat(path->chars, &info) != 0) return failure(errno, result);

  ObjObject *answer = beginAnswer(true);
  csObjectSetProperty(answer, "size", NUMBER_VAL((double)info.st_size));
  csObjectSetProperty(answer, "isDirectory", BOOL_VAL(S_ISDIR(info.st_mode)));
  csObjectSetProperty(answer, "isFile", BOOL_VAL(S_ISREG(info.st_mode)));
  /* Milliseconds since 1970, which is what a Date is here. */
  csObjectSetProperty(answer, "modifiedMs", NUMBER_VAL((double)info.st_mtime * 1000.0));
  return finishAnswer(answer, result);
}

static bool fsRemove(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.remove");
  if (path == NULL) return false;

  /* A directory needs rmdir and a file needs unlink, and asking which it is
   * first means a caller does not have to. An empty directory only: removing a
   * tree is a decision a program should have to write out. */
  struct stat info;
  if (stat(path->chars, &info) != 0) return failure(errno, result);
  int failed = S_ISDIR(info.st_mode) ? rmdir(path->chars) : unlink(path->chars);
  if (failed != 0) return failure(errno, result);

  ObjObject *answer = beginAnswer(true);
  return finishAnswer(answer, result);
}

static bool fsMakeDirectory(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.makeDirectory");
  if (path == NULL) return false;

  /* 0777 and let the umask narrow it, which is what every other tool does. */
  if (mkdir(path->chars, 0777) != 0) return failure(errno, result);
  ObjObject *answer = beginAnswer(true);
  return finishAnswer(answer, result);
}

/* The names in a directory, without `.` and `..` — a caller that wanted those
 * would have to filter them out every single time. Not sorted: the order a
 * filesystem reports is not meaningful, and pretending otherwise by sorting
 * here would hide that from a program that depends on it. `std:io` sorts. */
static bool fsList(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  ObjString *path = pathArgument(argCount, args, "fs.list");
  if (path == NULL) return false;

  DIR *directory = opendir(path->chars);
  if (directory == NULL) return failure(errno, result);

  ObjArray *entries = csArrayNew();
  csPushTempRoot((Obj *)entries);
  for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    ObjString *name = csStringCopy(entry->d_name, (int)strlen(entry->d_name));
    csNativeAppendRooted(entries, OBJ_VAL(name));
  }
  closedir(directory);

  ObjObject *answer = beginAnswer(true);
  csObjectSetProperty(answer, "entries", OBJ_VAL(entries));
  bool done = finishAnswer(answer, result);
  csPopTempRoot();
  return done;
}

void csNativeInstallFs(void) {
  ObjObject *ns = csNativeDefineNamespace("fs");
  csNativeDefineMethod(ns, "read", fsRead, 1);
  csNativeDefineMethod(ns, "write", fsWrite, 2);
  csNativeDefineMethod(ns, "append", fsAppend, 2);
  csNativeDefineMethod(ns, "exists", fsExists, 1);
  csNativeDefineMethod(ns, "stat", fsStat, 1);
  csNativeDefineMethod(ns, "remove", fsRemove, 1);
  csNativeDefineMethod(ns, "makeDirectory", fsMakeDirectory, 1);
  csNativeDefineMethod(ns, "list", fsList, 1);
  csObjectFreeze(ns);
}
