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
  OBJ_SHAPE,    /* an object layout; never visible from CScript */
  OBJ_CLASS,
  OBJ_BOUND_METHOD, /* a method captured away from its receiver */
} ObjType;

typedef struct ObjClosure ObjClosure;
typedef struct ObjClass ObjClass;

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

/* A property bag, in one of two representations.
 *
 * **Shape mode**, the normal one: the layout lives in a shared Shape and the
 * object holds nothing but a flat array of values. `o.x` on a known shape is a
 * pointer compare and an indexed load, which is what makes inline caching
 * worth doing. Insertion order — guaranteed for string keys since ES2015, and
 * relied on by anything that prints or serialises an object — is slot order,
 * so it costs nothing to preserve.
 *
 * **Dictionary mode**, for objects with more than CS_SHAPE_MAX_SLOTS
 * properties: a hash table plus a key list, which is what every object used to
 * be. An object that crosses the threshold converts once and stays converted.
 *
 * `shape == NULL` is the discriminator. */
typedef struct ObjObject {
  Obj obj;
  Shape *shape;
  ObjString *name; /* e.g. "console", used when printing the object */

  /* The class this is an instance of, or NULL for a plain object literal.
   *
   * Instances are ordinary ObjObjects rather than a separate type, which is
   * what makes classes cheap: fields go in slots, shapes are shared between
   * instances the way they are between literals, and every inline cache,
   * Object.keys and GC walk already works. A property that misses the shape
   * falls back to the class chain. */
  ObjClass *klass;

  /* Built-in namespaces refuse to be modified.
   *
   * JavaScript lets you write `Math.PI = 3` or replace `console.log`, and the
   * failures that causes are remote from the line that caused them. CScript
   * treats the standard library as part of the language rather than as an
   * object that happens to be lying around. User objects are never frozen. */
  bool frozen;
  union {
    struct {
      Value *values;
      int capacity;
    } slots;
    struct {
      Table table;
      ObjString **keys;
      int count;
      int capacity;
    } dictionary;
  } as;
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

struct ObjClosure {
  Obj obj;
  ObjFunction *function;
  ObjUpvalue **upvalues;
  int upvalueCount;
};

/* Methods are looked up through the superclass chain rather than copied down
 * into each subclass, so a method added to a base class is visible from every
 * subclass, and the chain is short enough that walking it costs little. */
struct ObjClass {
  Obj obj;
  ObjString *name;
  ObjClass *superclass; /* NULL for a base class */
  Table methods;
  Table statics;

  /* The constructor, or NULL. A class without one is constructed by the
   * nearest ancestor that has one, which is how `class Dog extends Animal {}`
   * still accepts Animal's arguments. */
  ObjClosure *initializer;

  /* A compiler-generated method holding this class's field initialisers, or
   * NULL when it declares no fields. Kept apart from the constructor so that a
   * class without a constructor still initialises its fields, and so a
   * subclass never has to remember to run its parent's. */
  ObjClosure *fieldInit;
};

/* Only ever created when a method is read without being called — `const f =
 * obj.method`. A method that is called goes through OP_INVOKE and allocates
 * nothing. */
typedef struct ObjBoundMethod {
  Obj obj;
  Value receiver;
  ObjClosure *method;
} ObjBoundMethod;

#define OBJ_TYPE(v)   (AS_OBJ(v)->type)

#define IS_STRING(v)   csIsObjType(v, OBJ_STRING)
#define IS_NATIVE(v)   csIsObjType(v, OBJ_NATIVE)
#define IS_OBJECT(v)   csIsObjType(v, OBJ_OBJECT)
#define IS_FUNCTION(v) csIsObjType(v, OBJ_FUNCTION)
#define IS_CLOSURE(v)  csIsObjType(v, OBJ_CLOSURE)
#define IS_ARRAY(v)    csIsObjType(v, OBJ_ARRAY)
#define IS_CLASS(v)    csIsObjType(v, OBJ_CLASS)
#define IS_BOUND_METHOD(v) csIsObjType(v, OBJ_BOUND_METHOD)

#define AS_STRING(v)   ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v)  (((ObjString *)AS_OBJ(v))->chars)
#define AS_NATIVE(v)   ((ObjNative *)AS_OBJ(v))
#define AS_OBJECT(v)   ((ObjObject *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_CLOSURE(v)  ((ObjClosure *)AS_OBJ(v))
#define AS_ARRAY(v)    ((ObjArray *)AS_OBJ(v))
#define AS_CLASS(v)    ((ObjClass *)AS_OBJ(v))
#define AS_BOUND_METHOD(v) ((ObjBoundMethod *)AS_OBJ(v))

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
ObjClass *csClassNew(ObjString *name);
ObjObject *csInstanceNew(ObjClass *klass);
ObjBoundMethod *csBoundMethodNew(Value receiver, ObjClosure *method);

/* Walks the superclass chain for a method. Returns NULL when nothing has it. */
ObjClosure *csClassFindMethod(ObjClass *klass, ObjString *name);

/* True when `klass` is `other` or descends from it — what `instanceof` asks. */
bool csClassDescendsFrom(const ObjClass *klass, const ObjClass *other);

/* Convenience for building namespace objects during startup. */
void csObjectSetProperty(ObjObject *object, const char *name, Value value);

/* Marks a namespace as immutable. Called once, after it is fully built. */
void csObjectFreeze(ObjObject *object);

/* Sets a property, recording insertion order for a key that is new. */
void csObjectPut(ObjObject *object, ObjString *key, Value value);

/* Reads a property. Everything outside object.c goes through these three
 * rather than touching the union, so the two representations stay an
 * implementation detail. */
bool csObjectGet(ObjObject *object, ObjString *key, Value *out);

/* Enumeration in insertion order — what Object.keys, JSON.stringify and
 * printing all need. `index` must be below csObjectCount(). */
int csObjectCount(const ObjObject *object);
ObjString *csObjectKeyAt(const ObjObject *object, int index);
Value csObjectValueAt(ObjObject *object, int index);

void csObjectPrint(Value value);
void csObjectFree(Obj *object);
void csObjectBlacken(Obj *object);

#endif /* CSCRIPT_OBJECT_H */
