/* object_ops.h — what can be done to a heap object.
 *
 * The macros that test and cast a Value, and the constructors and accessors
 * for every type in object.h. Included from the end of object.h rather than
 * separately, because nothing can use these without the layouts they are
 * about — so there is exactly one header to include and it is that one.
 */
#ifndef CSCRIPT_OBJECT_OPS_H
#define CSCRIPT_OBJECT_OPS_H

#include "cscript/object.h"

#define OBJ_TYPE(v) (AS_OBJ(v)->type)

#define IS_STRING(v) csIsObjType(v, OBJ_STRING)
#define IS_NATIVE(v) csIsObjType(v, OBJ_NATIVE)
#define IS_OBJECT(v) csIsObjType(v, OBJ_OBJECT)
#define IS_FUNCTION(v) csIsObjType(v, OBJ_FUNCTION)
#define IS_CLOSURE(v) csIsObjType(v, OBJ_CLOSURE)
#define IS_ARRAY(v) csIsObjType(v, OBJ_ARRAY)
#define IS_CLASS(v) csIsObjType(v, OBJ_CLASS)
#define IS_MODULE(v) csIsObjType(v, OBJ_MODULE)
#define IS_PROMISE(v) csIsObjType(v, OBJ_PROMISE)
#define IS_MAP(v) csIsObjType(v, OBJ_MAP)
#define IS_GENERATOR(v) csIsObjType(v, OBJ_GENERATOR)
#define IS_DATE(v) csIsObjType(v, OBJ_DATE)
#define IS_SYMBOL(v) csIsObjType(v, OBJ_SYMBOL)
#define IS_BIGINT(v) csIsObjType(v, OBJ_BIGINT)
#define AS_BIGINT(v) ((ObjBigInt *)AS_OBJ(v))
#define AS_SYMBOL(v) ((ObjSymbol *)AS_OBJ(v))
#define AS_DATE(v) ((ObjDate *)AS_OBJ(v))
#define AS_GENERATOR(v) ((ObjGenerator *)AS_OBJ(v))
#define IS_REGEX(v) csIsObjType(v, OBJ_REGEX)
#define IS_BOUND_METHOD(v) csIsObjType(v, OBJ_BOUND_METHOD)

/* Anything a call expression could name. Not a type in the checker's lattice —
 * a native, a closure and a bound method are three object kinds that happen to
 * share the one thing callers care about. */
#define csValueIsCallable(v) (IS_CLOSURE(v) || IS_NATIVE(v) || IS_BOUND_METHOD(v) || IS_CLASS(v))

#define AS_STRING(v) ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v) (((ObjString *)AS_OBJ(v))->chars)
#define AS_NATIVE(v) ((ObjNative *)AS_OBJ(v))
#define AS_OBJECT(v) ((ObjObject *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_CLOSURE(v) ((ObjClosure *)AS_OBJ(v))
#define AS_ARRAY(v) ((ObjArray *)AS_OBJ(v))
#define AS_CLASS(v) ((ObjClass *)AS_OBJ(v))
#define AS_MODULE(v) ((ObjModule *)AS_OBJ(v))
#define AS_PROMISE(v) ((ObjPromise *)AS_OBJ(v))
#define AS_MAP(v) ((ObjMap *)AS_OBJ(v))
#define AS_REGEX(v) ((ObjRegex *)AS_OBJ(v))
#define AS_BOUND_METHOD(v) ((ObjBoundMethod *)AS_OBJ(v))

static inline bool csIsObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

/* Interns a copy of `chars`. Returns an existing string when one matches. */
ObjString *csStringCopy(const char *chars, int length);
ObjString *csStringConcat(ObjString *a, ObjString *b);
ObjString *csStringTakeOwnership(char *chars, int length);

ObjNative *csNativeNew(NativeFn function, const char *name, int arity);
/* What a property may have done to it. A property that has never been through
 * `Object.defineProperty` has all three. */
#define CS_PROP_WRITABLE 1u
#define CS_PROP_ENUMERABLE 2u
#define CS_PROP_CONFIGURABLE 4u
#define CS_PROP_DEFAULT (CS_PROP_WRITABLE | CS_PROP_ENUMERABLE | CS_PROP_CONFIGURABLE)

ObjObject *csObjectNew(const char *name);

/* Makes room for `slots` properties without adding any.
 *
 * An instance is built to be filled: its constructor is about to add the
 * fields, and the first of those would otherwise grow the storage from nothing
 * — an allocation in the middle of the one path that most wants to be two
 * stores. Reserving up front is the same allocation moved earlier, and it is
 * what lets a cached add, in the interpreter and in compiled code, run without
 * one. */
void csObjectReserveSlots(ObjObject *object, int slots);

/* Drops an object out of shape mode, so the write fast path stops recognising
 * it. Needed once any of its properties is not writable. */
void csObjectLeaveShapeMode(ObjObject *object);

/* The attributes of one own property. CS_PROP_DEFAULT when nothing has said
 * otherwise, which is the answer for almost every property of almost every
 * object. */
unsigned csObjectAttributes(ObjObject *object, ObjString *key);
void csObjectSetAttributes(ObjObject *object, ObjString *key, unsigned attributes);

/* Should this key be listed by `Object.keys`, JSON, a spread or a print? */
bool csObjectIsEnumerable(ObjObject *object, ObjString *key);

/* Reads `key` from `object` or, failing that, from its prototype chain. False
 * when no object in the chain has it. */
bool csObjectGetInherited(ObjObject *object, ObjString *key, Value *out);

/* True when `object` or anything in its prototype chain has `key`. */
bool csObjectHasInherited(ObjObject *object, ObjString *key);

/* False when `prototype` is already in `object`'s chain, which would make the
 * chain a loop and every lookup on it non-terminating. */
bool csObjectSetPrototype(ObjObject *object, ObjObject *prototype);
#define CS_PARAM_UNSEEN 0
#define CS_PARAM_NUMBER 1
#define CS_PARAM_MIXED 2

ObjFunction *csFunctionNew(void);
ObjUpvalue *csUpvalueNew(Value *slot);
ObjClosure *csClosureNew(ObjFunction *function);
ObjArray *csArrayNew(void);
ObjClass *csClassNew(ObjString *name);

/* Creates a module with the built-ins already in scope. */
ObjModule *csModuleNew(ObjString *path);

ObjPromise *csPromiseNew(void);

ObjMap *csMapNew(bool isSet);

/* Rebuilds a map's index over the entries it already has, without allocating.
 * For the collector, which may not allocate and has just removed entries. */
void csMapReindexInPlace(ObjMap *map);

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

/* Wraps a fiber that has been set up but not started. */
ObjGenerator *csGeneratorNew(ObjFiber *fiber);

ObjDate *csDateNew(double ms);

ObjSymbol *csSymbolNew(ObjString *description);

/* Takes ownership of `value`'s limbs. */
ObjBigInt *csBigIntNew(BigInt value);

/* Settles a promise and queues whatever was waiting on it. Settling an already
 * settled promise does nothing, which is what makes a resolve function safe to
 * call twice. Resolving *with* a promise adopts its outcome instead of nesting.
 */
void csPromiseFulfill(ObjPromise *promise, Value value);
void csPromiseReject(ObjPromise *promise, Value reason);

/* Registers a reaction, running it as a microtask straight away when the
 * promise has already settled. */
void csPromiseAddReaction(ObjPromise *promise, Value onFulfilled, Value onRejected, ObjPromise *result);
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

/* Removes a property, answering whether it was there. Costs the object its
 * hidden class: see the comment on the definition. */
bool csObjectDelete(ObjObject *object, ObjString *key);

/* Private fields — `this.#count`.
 *
 * Kept beside the object rather than in it, because that is what "private"
 * means here: they take no shape slot, so they cannot be reached by
 * `Object.keys`, by `JSON.stringify`, by a subscript, or by anything else that
 * walks an object's properties. The table is allocated on the first write, so
 * an object with no private fields costs one NULL pointer. */
bool csObjectGetPrivate(ObjObject *object, ObjString *key, Value *out);

/* A named property on an array. See ObjArray.extras. */
bool csArrayGetExtra(ObjArray *array, ObjString *key, Value *out);
void csArrayPutExtra(ObjArray *array, const char *name, int length, Value value);
void csObjectPutPrivate(ObjObject *object, ObjString *key, Value value);
bool csObjectDeletePrivate(ObjObject *object, ObjString *key);

/* Enumeration in insertion order — what Object.keys, JSON.stringify and
 * printing all need. `index` must be below csObjectCount(). */
int csObjectCount(const ObjObject *object);
ObjString *csObjectKeyAt(const ObjObject *object, int index);
Value csObjectValueAt(ObjObject *object, int index);

void csObjectPrint(Value value);
void csObjectFree(Obj *object);
void csObjectBlacken(Obj *object);

#endif /* CSCRIPT_OBJECT_OPS_H */
