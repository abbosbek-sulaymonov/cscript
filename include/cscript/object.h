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
  OBJ_MODULE,       /* one source file: its own top-level scope */
  OBJ_PROMISE,
  OBJ_FIBER, /* a suspendable call: an async function's own stack */
  OBJ_MAP,   /* also Set: a set is a map that stores only its keys */
  OBJ_REGEX,
} ObjType;

typedef struct ObjPromise ObjPromise;

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

/* One source file, and the scope its top level lives in.
 *
 * Before modules there was a single globals table, so two files could not be
 * combined without their top-level names colliding. A module owns its own.
 * Built-ins are copied in when it is created, which keeps every global lookup
 * one uniform mechanism rather than a lookup with a fallback behind it. */
struct ObjModule {
  Obj obj;
  ObjString *path; /* resolved and absolute: the registry key */
  Table globals;
  Table globalConsts;

  /* Used as a set. Which names this module allows to be imported — the values
   * are read through to `globals`, so a binding stays live and nothing has to
   * be copied when the module finishes running. */
  Table exports;

  /* The compiled top level, kept only until it runs. */
  ObjFunction *body;

  /* Loaded but not finished: a module reached again while this is set closes
   * an import cycle. */
  bool loading;
  bool executed;
};

/* One `.then` waiting on a promise: what to run when it settles, and the
 * promise that gets whatever that produces. Both handlers may be undefined,
 * in which case the outcome passes straight through — which is what makes
 * `.then(f)` forward a rejection and `.catch(g)` forward a value. */
typedef struct {
  Value onFulfilled;
  Value onRejected;
  ObjPromise *result;

  /* Set instead of a handler when the waiter is a suspended `await`. */
  struct ObjFiber *fiber;

  /* `.finally` runs its handler either way and then passes the *original*
   * outcome along, which no combination of the two handlers above can say:
   * they replace the outcome with whatever they return. */
  bool isFinally;

  /* Microtask turns this outcome still owes before it is delivered; see
   * Microtask in vm.h. */
  int extraHops;

  /* Set instead of the handlers when the waiter is Promise.all or .race rather
   * than user code. A combinator has to count settlements and place results by
   * position, which no single-argument callback can do — and building it out
   * of user-visible closures would mean allocating two per element. The state
   * is [results, remaining, target]; index -1 means the first to settle wins.
   */
  struct ObjArray *combineState;
  int combineIndex;
} Reaction;

typedef enum {
  PROMISE_PENDING,
  PROMISE_FULFILLED,
  PROMISE_REJECTED,
} PromiseState;

struct ObjPromise {
  Obj obj;
  PromiseState state;
  Value value; /* the fulfilment value, or the rejection reason */

  Reaction *reactions;
  int reactionCount;
  int reactionCapacity;

  /* Whether anything ever asked what happened. A rejection nothing is
   * listening to is a bug the program would otherwise swallow, so it is
   * reported when the event loop runs dry. */
  bool handled;
};

typedef enum {
  FIBER_READY,     /* set up, never started */
  FIBER_RUNNING,
  FIBER_SUSPENDED, /* waiting on a promise */
  FIBER_DONE,
} FiberState;

struct ObjFiber {
  Obj obj;

  /* The saved execution state, mirroring the fields the VM holds inline while
   * this fiber is the one running. */
  Value *stack;
  Value *stackTop;
  int stackCapacity;
  struct CallFrame *frames;
  int frameCount;
  struct ExceptionHandler *handlers;
  int handlerCount;
  struct ObjUpvalue *openUpvalues;

  /* What the async function call handed back to its caller, settled when the
   * body returns or throws. */
  ObjPromise *promise;
  FiberState state;
};

/* One entry of a Map or Set. `present` is false for a tombstone: a deleted
 * entry keeps its slot so that insertion order survives deletion, which
 * JavaScript guarantees and real code depends on when iterating. */
typedef struct {
  Value key;
  Value value;
  bool present;
} MapEntry;

/* A Map, and also a Set — a set is a map that only ever looks at its keys, and
 * sharing the implementation is cheaper than keeping two of the same thing in
 * step.
 *
 * Unlike an object, the keys are *any* value rather than interned strings, so
 * this cannot reuse Table: it needs a hash and an equality for numbers,
 * booleans and object identity as well. Entries are kept in a dense array in
 * insertion order, with a separate open-addressing index from hash to slot —
 * which is what makes iteration ordered and lookup constant at the same time. */
typedef struct ObjMap {
  Obj obj;

  MapEntry *entries; /* insertion order, tombstones included */
  int count;         /* slots used in `entries`, live or not */
  int capacity;
  int liveCount;     /* what .size reports */

  int *index;        /* hash slot -> index into `entries`, or -1 */
  int indexCapacity;

  bool isSet;
} ObjMap;

/* A compiled regular expression.
 *
 * The compiled program is owned here rather than shared, because a literal in
 * a loop is compiled once — at the point the constant is created — and the
 * object it produces lives as long as the code that names it.
 *
 * `lastIndex` is where a `g` pattern resumes. It is per-object and mutable,
 * which is JavaScript's design and its most notorious sharp edge: a global
 * regex reused across calls carries its position with it. */
typedef struct ObjRegex {
  Obj obj;
  struct Regex *program;
  ObjString *source; /* the pattern, without the slashes */
  ObjString *flags;
  int lastIndex;
  bool global;
  bool ignoreCase;
  bool multiline;
} ObjRegex;

/* Compiled user code. Every function body is its own chunk, and the top level
 * of a module is itself a function — which is what lets the VM run modules and
 * calls through exactly one mechanism. */
struct ObjFunction {
  Obj obj;
  int arity;
  int upvalueCount;
  Chunk chunk;
  ObjString *name; /* NULL for the implicit top-level function */
  /* The module this was compiled in, which is where its global reads and
   * writes resolve. Every function in a file shares it. */
  ObjModule *module;

  /* Calling this returns a promise and runs the body on a fiber of its own, so
   * an `await` inside it can suspend without disturbing its caller. */
  bool isAsync;

  /* Tiering. `hotness` counts calls and loop back-edges together, because a
   * function called a million times and one called once around a millionfold
   * loop are equally worth compiling. See jit.h. */
  int hotness;
  int jitState;
  void *jitCode; /* NULL until a backend exists */

  /* Set while compiling: how many operations the declared types let the
   * compiler specialise, against how many it had to leave generic. The ratio
   * is what says whether a type-directed compiler is worth building. */
  int typedSites;
  int genericSites;
};

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

  /* Accessors, kept apart from methods so a property read can tell a stored
   * field from a computed one without a flag on every lookup. An accessor
   * never enters an instance's shape, so the inline caches are untouched: a
   * shape hit is always a real field, and a miss is where accessors are
   * looked for. */
  Table getters;
  Table setters;

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

/* A callable paired with the receiver it belongs to.
 *
 * Created when a method is read without being called — `const f = obj.method`
 * — since a method that *is* called goes through OP_INVOKE and allocates
 * nothing. It doubles as the way a built-in carries state: natives already
 * take a receiver, so binding one to a value gives a C function a closure
 * without a second object type. That is what `new Promise(resolve, reject)`
 * hands the executor. */
typedef struct ObjBoundMethod {
  Obj obj;
  Value receiver;
  Obj *method; /* an ObjClosure or an ObjNative */
} ObjBoundMethod;

#define OBJ_TYPE(v)   (AS_OBJ(v)->type)

#define IS_STRING(v)   csIsObjType(v, OBJ_STRING)
#define IS_NATIVE(v)   csIsObjType(v, OBJ_NATIVE)
#define IS_OBJECT(v)   csIsObjType(v, OBJ_OBJECT)
#define IS_FUNCTION(v) csIsObjType(v, OBJ_FUNCTION)
#define IS_CLOSURE(v)  csIsObjType(v, OBJ_CLOSURE)
#define IS_ARRAY(v)    csIsObjType(v, OBJ_ARRAY)
#define IS_CLASS(v)    csIsObjType(v, OBJ_CLASS)
#define IS_MODULE(v)   csIsObjType(v, OBJ_MODULE)
#define IS_PROMISE(v)  csIsObjType(v, OBJ_PROMISE)
#define IS_MAP(v)      csIsObjType(v, OBJ_MAP)
#define IS_REGEX(v)    csIsObjType(v, OBJ_REGEX)
#define IS_BOUND_METHOD(v) csIsObjType(v, OBJ_BOUND_METHOD)

#define AS_STRING(v)   ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v)  (((ObjString *)AS_OBJ(v))->chars)
#define AS_NATIVE(v)   ((ObjNative *)AS_OBJ(v))
#define AS_OBJECT(v)   ((ObjObject *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_CLOSURE(v)  ((ObjClosure *)AS_OBJ(v))
#define AS_ARRAY(v)    ((ObjArray *)AS_OBJ(v))
#define AS_CLASS(v)    ((ObjClass *)AS_OBJ(v))
#define AS_MODULE(v)   ((ObjModule *)AS_OBJ(v))
#define AS_PROMISE(v)  ((ObjPromise *)AS_OBJ(v))
#define AS_MAP(v)      ((ObjMap *)AS_OBJ(v))
#define AS_REGEX(v)    ((ObjRegex *)AS_OBJ(v))
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

/* Creates a module with the built-ins already in scope. */
ObjModule *csModuleNew(ObjString *path);

ObjPromise *csPromiseNew(void);

ObjMap *csMapNew(bool isSet);

/* Compiles a pattern into a regex object. Returns NULL after reporting a
 * runtime error when the pattern is malformed. */
ObjRegex *csRegexObjectNew(ObjString *source, ObjString *flags);

/* What iterating a Map or Set yields: a Set's values, or a Map's [key, value]
 * pairs. Both `for...of` and spread need it, and they must agree. */
ObjArray *csMapToArray(ObjMap *map);

/* All four answer in constant time. `csMapGet` writes through `out` and
 * returns whether the key was there at all, which is how a stored `undefined`
 * stays distinguishable from a missing key. */
bool csMapGet(ObjMap *map, Value key, Value *out);
void csMapSet(ObjMap *map, Value key, Value value);
bool csMapHas(ObjMap *map, Value key);
bool csMapDelete(ObjMap *map, Value key);
void csMapClear(ObjMap *map);

/* Keys compare by SameValueZero: `===`, except that NaN matches itself and
 * -0 matches +0. That is the rule Map and Set use, and it differs from `===`
 * in exactly the two places where `===` is surprising. */
bool csValuesSameValueZero(Value a, Value b);

/* A suspendable call.
 *
 * `await` has to stop a running function and start it again later. Copying its
 * slots off the value stack and back would be cheaper, but upvalues hold raw
 * pointers *into* that stack: move a captured local and the closure that
 * captured it reads freed memory. So a suspendable call gets a stack of its
 * own, and nothing ever moves.
 *
 * The active fiber's state lives inline in the VM — see vm.h — so the
 * interpreter loop never pays for the indirection. Suspending copies it out to
 * here and the caller's back in. */
typedef struct ObjFiber ObjFiber;
ObjFiber *csFiberNew(void);

/* Settles a promise and queues whatever was waiting on it. Settling an already
 * settled promise does nothing, which is what makes a resolve function safe to
 * call twice. Resolving *with* a promise adopts its outcome instead of nesting.
 */
void csPromiseFulfill(ObjPromise *promise, Value value);
void csPromiseReject(ObjPromise *promise, Value reason);

/* Registers a reaction, running it as a microtask straight away when the
 * promise has already settled. */
void csPromiseAddReaction(ObjPromise *promise, Value onFulfilled, Value onRejected,
                          ObjPromise *result);
ObjObject *csInstanceNew(ObjClass *klass);
ObjBoundMethod *csBoundMethodNew(Value receiver, Obj *method);

/* Walks the superclass chain for a method. Returns NULL when nothing has it. */
ObjClosure *csClassFindMethod(ObjClass *klass, ObjString *name);

/* The same walk over the accessor tables. */
ObjClosure *csClassFindGetter(ObjClass *klass, ObjString *name);
ObjClosure *csClassFindSetter(ObjClass *klass, ObjString *name);

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
