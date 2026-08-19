/* object.h — heap objects and their shared GC header. */
#ifndef CSCRIPT_OBJECT_H
#define CSCRIPT_OBJECT_H

#include "cscript/common.h"
#include "cscript/table.h"
#include "cscript/value.h"

typedef enum {
  OBJ_STRING,
  OBJ_NATIVE, /* a function implemented in C */
  OBJ_OBJECT, /* a property bag, e.g. the `console` namespace */
} ObjType;

/* Every heap object starts with this header, so the collector can walk the
 * allocation list without knowing the concrete type. */
struct Obj {
  ObjType type;
  bool isMarked;
  struct Obj *next; /* intrusive list of every live object */
};

/* Strings are immutable and interned, so equality is a pointer compare and the
 * hash is computed once at creation. The characters are stored inline, in the
 * same allocation as the header. */
struct ObjString {
  Obj obj;
  int length;
  uint32_t hash;
  char chars[]; /* flexible array member, NUL-terminated */
};

/* Returns false to signal a runtime error; the callee reports it. `args` points
 * at the first argument on the VM stack, and the result goes through `result`. */
typedef bool (*NativeFn)(int argCount, Value *args, Value *result);

typedef struct {
  Obj obj;
  NativeFn function;
  ObjString *name; /* for error messages and disassembly */
  int arity;       /* -1 means variadic */
} ObjNative;

typedef struct {
  Obj obj;
  Table properties;
  ObjString *name; /* e.g. "console", used when printing the object */
} ObjObject;

#define OBJ_TYPE(v)   (AS_OBJ(v)->type)

#define IS_STRING(v)  csIsObjType(v, OBJ_STRING)
#define IS_NATIVE(v)  csIsObjType(v, OBJ_NATIVE)
#define IS_OBJECT(v)  csIsObjType(v, OBJ_OBJECT)

#define AS_STRING(v)  ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v) (((ObjString *)AS_OBJ(v))->chars)
#define AS_NATIVE(v)  ((ObjNative *)AS_OBJ(v))
#define AS_OBJECT(v)  ((ObjObject *)AS_OBJ(v))

static inline bool csIsObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

/* Interns a copy of `chars`. Returns an existing string when one matches. */
ObjString *csStringCopy(const char *chars, int length);
ObjString *csStringConcat(ObjString *a, ObjString *b);
ObjString *csStringTakeOwnership(char *chars, int length);

ObjNative *csNativeNew(NativeFn function, const char *name, int arity);
ObjObject *csObjectNew(const char *name);

/* Convenience for building namespace objects during startup. */
void csObjectSetProperty(ObjObject *object, const char *name, Value value);

void csObjectPrint(Value value);
void csObjectFree(Obj *object);
void csObjectBlacken(Obj *object);

#endif /* CSCRIPT_OBJECT_H */
