/* object.h — heap objects and their shared GC header. */
#ifndef CSCRIPT_OBJECT_H
#define CSCRIPT_OBJECT_H

#include "cscript/common.h"
#include "cscript/value.h"

typedef enum {
  OBJ_STRING,
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

#define OBJ_TYPE(v)   (AS_OBJ(v)->type)
#define IS_STRING(v)  csIsObjType(v, OBJ_STRING)
#define AS_STRING(v)  ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v) (((ObjString *)AS_OBJ(v))->chars)

static inline bool csIsObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

/* Interns a copy of `chars`. Returns an existing string when one matches. */
ObjString *csStringCopy(const char *chars, int length);

/* Interns the concatenation of two strings. */
ObjString *csStringConcat(ObjString *a, ObjString *b);

/* Interns a C string the caller owns; frees `chars` on the way out. */
ObjString *csStringTakeOwnership(char *chars, int length);

void csObjectPrint(Value value);
void csObjectFree(Obj *object);

#endif /* CSCRIPT_OBJECT_H */
