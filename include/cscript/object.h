/* object.h — heap objects and their shared GC header. */
#ifndef CSCRIPT_OBJECT_H
#define CSCRIPT_OBJECT_H

#include "cscript/chunk.h"
#include "cscript/common.h"
#include "cscript/table.h"
#include "cscript/value.h"

typedef enum {
  OBJ_STRING,
  OBJ_NATIVE,   /* a function implemented in C */
  OBJ_OBJECT,   /* a property bag, e.g. the `console` namespace */
  OBJ_FUNCTION, /* compiled user code: a chunk plus its metadata */
  OBJ_UPVALUE,  /* a local captured by a nested function */
  OBJ_CLOSURE,  /* a function paired with the upvalues it captured */
  OBJ_ARRAY,    /* a dense, growable list of values */
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
 * at the first argument on the VM stack, and the result goes through `result`.
 *
 * `receiver` is the value a method was invoked on — the array in `xs.push(1)`.
 * Plain functions are called with `undefined` there and ignore it. One
 * signature for both keeps a single call path in the VM. */
typedef bool (*NativeFn)(Value receiver, int argCount, Value *args, Value *result);

typedef struct ObjNative {
  Obj obj;
  NativeFn function;
  ObjString *name; /* for error messages and disassembly */
  int arity;       /* -1 means variadic */

  /* Static properties hung off a callable, so `Number(x)` and
   * `Number.isInteger` can both work. In JavaScript functions are objects and
   * carry properties directly; here that would cost a table on every native,
   * so the few that need one point at it instead. NULL for almost all. */
  struct ObjObject *statics;
} ObjNative;

/* A property bag.
 *
 * The hash table answers lookups; `keys` records insertion order, because
 * JavaScript has guaranteed it for string keys since ES2015 and real code
 * depends on it when printing or serialising an object. Keeping both means one
 * extra pointer per property, which is cheaper than an ordered map. */
typedef struct ObjObject {
  Obj obj;
  Table properties;
  ObjString **keys;
  int keyCount;
  int keyCapacity;
  ObjString *name; /* e.g. "console", used when printing the object */
} ObjObject;

/* Compiled user code. Every function body is its own chunk, and the top level
 * of a script is itself a function — which is what lets the VM run scripts and
 * calls through exactly one mechanism. */
typedef struct ObjFunction {
  Obj obj;
  int arity;
  int upvalueCount;
  Chunk chunk;
  ObjString *name; /* NULL for the implicit top-level function */
} ObjFunction;

/* A captured variable.
 *
 * While the enclosing call is still on the stack, `location` points straight at
 * that stack slot, so reads and writes are shared with it. When the call
 * returns, the value is copied into `closed` and `location` is repointed there,
 * so the closure keeps working after its defining scope is gone. */
typedef struct ObjUpvalue {
  Obj obj;
  Value *location;
  Value closed;
  struct ObjUpvalue *next; /* the VM's list of still-open upvalues */
} ObjUpvalue;

/* A function value. Every user function is called through a closure, even when
 * it captures nothing, so the VM needs only one calling path. */
/* A dense array. Sparse arrays and holes are deliberately not supported: they
 * are the reason JavaScript engines need a second, slower representation. */
typedef struct ObjArray {
  Obj obj;
  ValueArray elements;
  /* Set only on the throwaway array a spread element produces, so the literal
   * being built knows to splice it rather than nest it. Never observable from
   * CScript: the marked array exists for exactly one instruction. */
  bool isSpreadMarker;
} ObjArray;

typedef struct ObjClosure {
  Obj obj;
  ObjFunction *function;
  ObjUpvalue **upvalues;
  int upvalueCount;
} ObjClosure;

#define OBJ_TYPE(v)   (AS_OBJ(v)->type)

#define IS_STRING(v)   csIsObjType(v, OBJ_STRING)
#define IS_NATIVE(v)   csIsObjType(v, OBJ_NATIVE)
#define IS_OBJECT(v)   csIsObjType(v, OBJ_OBJECT)
#define IS_FUNCTION(v) csIsObjType(v, OBJ_FUNCTION)
#define IS_CLOSURE(v)  csIsObjType(v, OBJ_CLOSURE)
#define IS_ARRAY(v)    csIsObjType(v, OBJ_ARRAY)

#define AS_STRING(v)   ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v)  (((ObjString *)AS_OBJ(v))->chars)
#define AS_NATIVE(v)   ((ObjNative *)AS_OBJ(v))
#define AS_OBJECT(v)   ((ObjObject *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_CLOSURE(v)  ((ObjClosure *)AS_OBJ(v))
#define AS_ARRAY(v)    ((ObjArray *)AS_OBJ(v))

static inline bool csIsObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

/* Interns a copy of `chars`. Returns an existing string when one matches. */
ObjString *csStringCopy(const char *chars, int length);
ObjString *csStringConcat(ObjString *a, ObjString *b);
ObjString *csStringTakeOwnership(char *chars, int length);

ObjNative *csNativeNew(NativeFn function, const char *name, int arity);
ObjObject *csObjectNew(const char *name);
ObjFunction *csFunctionNew(void);
ObjUpvalue *csUpvalueNew(Value *slot);
ObjClosure *csClosureNew(ObjFunction *function);
ObjArray *csArrayNew(void);

/* Convenience for building namespace objects during startup. */
void csObjectSetProperty(ObjObject *object, const char *name, Value value);

/* Sets a property, recording insertion order for a key that is new. */
void csObjectPut(ObjObject *object, ObjString *key, Value value);

void csObjectPrint(Value value);
void csObjectFree(Obj *object);
void csObjectBlacken(Obj *object);

#endif /* CSCRIPT_OBJECT_H */
