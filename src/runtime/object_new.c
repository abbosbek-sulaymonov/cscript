/* object_new.c — the constructors for everything that is not a property bag.
 *
 * Each is the same three steps — allocate, register, initialise — and the
 * order matters: an object is on the collector's list before its fields are
 * filled in, so anything that allocates while initialising has to keep it
 * rooted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/regex.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

#include "runtime/object_internal.h"

ObjFunction *csFunctionNew(void) {
  ObjFunction *function = CS_ALLOCATE(ObjFunction, 1);
  csObjectRegister((Obj *)function, OBJ_FUNCTION);
  function->arity = 0;
  function->paramCount = 0;
  function->hasRest = false;
  function->upvalueCount = 0;
  function->name = NULL;
  function->module = NULL;
  function->isAsync = false;
  function->isMethod = false;
  function->isGenerator = false;
  function->usesThis = false;
  function->paramTypes = NULL;
  function->observedParams = NULL;
  function->hotness = 0;
  function->jitState = 0; /* JIT_INTERPRETED */
  function->jitCode = NULL;
  function->jitSlot = -1;
  function->jitOsrRefusedAt = -1;
  function->typedSites = 0;
  function->genericSites = 0;
  csChunkInit(&function->chunk);
  return function;
}

ObjUpvalue *csUpvalueNew(Value *slot) {
  ObjUpvalue *upvalue = CS_ALLOCATE(ObjUpvalue, 1);
  csObjectRegister((Obj *)upvalue, OBJ_UPVALUE);
  upvalue->location = slot;
  upvalue->closed = NULL_VAL;
  upvalue->next = NULL;
  return upvalue;
}

ObjClosure *csClosureNew(ObjFunction *function) {
  /* Allocate the upvalue array first and clear it: if the closure allocation
   * triggers a collection, the collector must not walk uninitialised slots. */
  ObjUpvalue **upvalues = CS_ALLOCATE(ObjUpvalue *, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) upvalues[i] = NULL;

  ObjClosure *closure = CS_ALLOCATE(ObjClosure, 1);
  csObjectRegister((Obj *)closure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  closure->prototype = NULL;
  return closure;
}

ObjObject *csClosurePrototype(ObjClosure *closure) {
  if (closure->prototype != NULL) return closure->prototype;

  csPushTempRoot((Obj *)closure);
  ObjObject *prototype = csObjectNew(closure->function->name != NULL
                                         ? closure->function->name->chars
                                         : "Object");
  closure->prototype = prototype;

  /* `F.prototype.constructor === F`, filed in the table beside the shape
   * rather than as an ordinary property. That table exists for names nothing
   * that enumerates may see, which is exactly what `constructor` is: JavaScript
   * makes it non-enumerable, so `Object.keys(F.prototype)` is empty there and
   * has to be empty here. The leading hash is what keeps it unwritable from
   * source — a `#name` outside a class body does not compile. */
  csPushTempRoot((Obj *)prototype);
  ObjString *key = csStringCopy("#constructor", 12);
  csPushTempRoot((Obj *)key);
  csObjectPutPrivate(prototype, key, OBJ_VAL(closure));
  csPopTempRoot();
  csPopTempRoot();
  csPopTempRoot();
  return prototype;
}

ObjModule *csModuleNew(ObjString *path) {
  csPushTempRoot((Obj *)path);
  ObjModule *module = CS_ALLOCATE(ObjModule, 1);
  csObjectRegister((Obj *)module, OBJ_MODULE);
  module->path = path;
  module->body = NULL;
  module->namespaceView = NULL;
  module->loading = false;
  module->executed = false;
  csTableInit(&module->globals);
  csTableInit(&module->globalConsts);
  csTableInit(&module->exports);

  /* The built-ins are in scope in every module. Copying them costs one table
   * of about thirty entries per file and keeps global lookup a single hash
   * rather than a hash with a fallback behind it. */
  csPushTempRoot((Obj *)module);
  csTableAddAll(&vm.builtins, &module->globals);
  csTableAddAll(&vm.builtinConsts, &module->globalConsts);
  csPopTempRoot();

  csPopTempRoot();
  return module;
}

ObjRegex *csRegexObjectNew(ObjString *source, ObjString *flags) {
  bool global = false, ignoreCase = false, multiline = false, dotAll = false;
  for (int i = 0; i < flags->length; i++) {
    switch (flags->chars[i]) {
      case 'g': global = true; break;
      case 'i': ignoreCase = true; break;
      case 'm': multiline = true; break;
      case 's': dotAll = true; break;
      default:
        csVMRuntimeError("unsupported regular expression flag '%c'", flags->chars[i]);
        return NULL;
    }
  }

  char error[128];
  Regex *program = csRegexCompile(source->chars, source->length, ignoreCase, multiline,
                                  dotAll, error, sizeof error);
  if (program == NULL) {
    csVMRuntimeError("bad regular expression /%s/: %s", source->chars, error);
    return NULL;
  }

  csPushTempRoot((Obj *)source);
  csPushTempRoot((Obj *)flags);
  ObjRegex *regex = CS_ALLOCATE(ObjRegex, 1);
  csObjectRegister((Obj *)regex, OBJ_REGEX);
  regex->program = program;
  regex->source = source;
  regex->flags = flags;
  regex->lastIndex = 0;
  regex->global = global;
  regex->ignoreCase = ignoreCase;
  regex->multiline = multiline;
  csPopTempRoot();
  csPopTempRoot();
  return regex;
}

ObjPromise *csPromiseNew(void) {
  ObjPromise *promise = CS_ALLOCATE(ObjPromise, 1);
  csObjectRegister((Obj *)promise, OBJ_PROMISE);
  promise->state = PROMISE_PENDING;
  promise->value = UNDEFINED_VAL;
  promise->reactions = NULL;
  promise->reactionCount = 0;
  promise->reactionCapacity = 0;
  promise->handled = false;
  return promise;
}

/* Settling queues the waiting reactions rather than running them, which is
 * what makes `.then` always asynchronous — and is the whole reason a promise
 * and a plain callback behave differently. */
static void settle(ObjPromise *promise, PromiseState state, Value value) {
  if (promise->state != PROMISE_PENDING) return;

  promise->state = state;
  promise->value = value;

  for (int i = 0; i < promise->reactionCount; i++) {
    Reaction *reaction = &promise->reactions[i];
    if (reaction->fiber != NULL) {
      csVMQueueMicrotask(UNDEFINED_VAL, value, NULL, state == PROMISE_REJECTED);
      vm.microtasks[vm.microtaskCount - 1].fiber = reaction->fiber;
      continue;
    }
    if (reaction->combineState != NULL) {
      csVMQueueCombine(reaction->combineState, reaction->combineIndex, value,
                       state == PROMISE_REJECTED);
      continue;
    }
    Value handler =
        state == PROMISE_FULFILLED ? reaction->onFulfilled : reaction->onRejected;
    csVMQueueMicrotask(handler, value, reaction->result,
                       state == PROMISE_REJECTED);
    csVMLastMicrotask()->isFinally = reaction->isFinally;
    csVMLastMicrotask()->extraHops = reaction->extraHops;
  }

  CS_FREE_ARRAY(Reaction, promise->reactions, promise->reactionCapacity);
  promise->reactions = NULL;
  promise->reactionCount = 0;
  promise->reactionCapacity = 0;

  if (state == PROMISE_REJECTED && !promise->handled) csVMNoteRejection(promise);
}

/* Resolving *with* a promise adopts its outcome rather than nesting, so
 * `return somePromise` inside a `.then` flattens the way it should. */
void csPromiseFulfill(ObjPromise *promise, Value value) {
  if (promise->state != PROMISE_PENDING) return;

  if (IS_PROMISE(value)) {
    ObjPromise *inner = AS_PROMISE(value);
    csPushTempRoot((Obj *)promise);
    csPromiseAddReaction(inner, UNDEFINED_VAL, UNDEFINED_VAL, promise);
    /* Adopting costs a turn of its own, because the specification resolves a
     * promise with a promise through a job rather than by copying its state. */
    if (inner->state == PROMISE_PENDING) {
      inner->reactions[inner->reactionCount - 1].extraHops = 1;
    } else {
      csVMLastMicrotask()->extraHops = 1;
    }
    csPopTempRoot();
    return;
  }
  settle(promise, PROMISE_FULFILLED, value);
}

void csPromiseReject(ObjPromise *promise, Value reason) {
  settle(promise, PROMISE_REJECTED, reason);
}

void csPromiseAddReaction(ObjPromise *promise, Value onFulfilled, Value onRejected,
                          ObjPromise *result) {
  promise->handled = true;

  if (promise->state != PROMISE_PENDING) {
    Value handler =
        promise->state == PROMISE_FULFILLED ? onFulfilled : onRejected;
    csVMQueueMicrotask(handler, promise->value, result,
                       promise->state == PROMISE_REJECTED);
    return;
  }

  if (promise->reactionCapacity < promise->reactionCount + 1) {
    int oldCapacity = promise->reactionCapacity;
    promise->reactionCapacity = CS_GROW_CAPACITY(oldCapacity);
    promise->reactions = CS_GROW_ARRAY(Reaction, promise->reactions, oldCapacity,
                                       promise->reactionCapacity);
  }
  Reaction *reaction = &promise->reactions[promise->reactionCount++];
  reaction->onFulfilled = onFulfilled;
  reaction->onRejected = onRejected;
  reaction->result = result;
  reaction->combineState = NULL;
  reaction->combineIndex = 0;
  reaction->fiber = NULL;
  reaction->isFinally = false;
  reaction->extraHops = 0;
}

ObjFiber *csFiberNew(void) {
  /* Allocated before the fiber itself and cleared, so a collection triggered
   * partway through never walks uninitialised slots. */
  Value *stack = CS_ALLOCATE(Value, CS_FIBER_STACK);
  for (int i = 0; i < CS_FIBER_STACK; i++) stack[i] = UNDEFINED_VAL;
  CallFrame *frames = CS_ALLOCATE(CallFrame, CS_FIBER_FRAMES);
  ExceptionHandler *handlers = CS_ALLOCATE(ExceptionHandler, CS_FIBER_HANDLERS);

  ObjFiber *fiber = CS_ALLOCATE(ObjFiber, 1);
  csObjectRegister((Obj *)fiber, OBJ_FIBER);
  fiber->stack = stack;
  fiber->stackTop = stack;
  fiber->stackCapacity = CS_FIBER_STACK;
  fiber->frames = frames;
  fiber->frameCount = 0;
  fiber->handlers = handlers;
  fiber->handlerCount = 0;
  fiber->openUpvalues = NULL;
  fiber->promise = NULL;
  fiber->state = FIBER_READY;
  fiber->generator = NULL;
  return fiber;
}

ObjBigInt *csBigIntNew(BigInt value) {
  ObjBigInt *number = CS_ALLOCATE(ObjBigInt, 1);
  csObjectRegister((Obj *)number, OBJ_BIGINT);
  number->value = value;
  return number;
}

ObjSymbol *csSymbolNew(ObjString *description) {
  if (description != NULL) csPushTempRoot((Obj *)description);

  /* The filing name. A counter rather than the address, so it survives a
   * collection moving nothing but staying reproducible run to run — and the
   * leading space is what no source can write. */
  static long long nextId = 0;
  char name[48];
  int length = snprintf(name, sizeof name, " symbol%lld", nextId++);
  ObjString *key = csStringCopy(name, length);
  csPushTempRoot((Obj *)key);

  ObjSymbol *symbol = CS_ALLOCATE(ObjSymbol, 1);
  csObjectRegister((Obj *)symbol, OBJ_SYMBOL);
  symbol->description = description;
  symbol->key = key;
  symbol->registered = false;

  /* Filed both ways: the symbol knows its key, and the key finds the symbol
   * again — which is the only way `Object.getOwnPropertySymbols` can say what
   * a property belongs to. */
  csPushTempRoot((Obj *)symbol);
  csTableSet(&vm.symbolsByKey, key, OBJ_VAL(symbol));
  csPopTempRoot();

  csPopTempRoot();
  if (description != NULL) csPopTempRoot();
  return symbol;
}

ObjDate *csDateNew(double ms) {
  ObjDate *date = CS_ALLOCATE(ObjDate, 1);
  csObjectRegister((Obj *)date, OBJ_DATE);
  date->ms = ms;
  return date;
}

ObjGenerator *csGeneratorNew(ObjFiber *fiber) {
  csPushTempRoot((Obj *)fiber);
  ObjGenerator *generator = CS_ALLOCATE(ObjGenerator, 1);
  csObjectRegister((Obj *)generator, OBJ_GENERATOR);
  generator->fiber = fiber;
  generator->yielded = UNDEFINED_VAL;
  generator->done = false;
  generator->running = false;
  generator->isAsync = false;
  generator->pendingResult = NULL;
  fiber->generator = generator;
  csPopTempRoot();
  return generator;
}

ObjClass *csClassNew(ObjString *name) {
  csPushTempRoot((Obj *)name);
  ObjClass *klass = CS_ALLOCATE(ObjClass, 1);
  csObjectRegister((Obj *)klass, OBJ_CLASS);
  klass->name = name;
  klass->superclass = NULL;
  klass->initializer = NULL;
  klass->fieldInit = NULL;
  csTableInit(&klass->methods);
  csTableInit(&klass->statics);
  csTableInit(&klass->getters);
  csTableInit(&klass->setters);
  csTableInit(&klass->staticGetters);
  csTableInit(&klass->staticSetters);
  klass->isAccessorHolder = false;
  csPopTempRoot();
  return klass;
}

ObjObject *csInstanceNew(ObjClass *klass) {
  csPushTempRoot((Obj *)klass);
  ObjObject *instance = CS_ALLOCATE(ObjObject, 1);
  csObjectRegister((Obj *)instance, OBJ_OBJECT);
  instance->name = klass->name;
  instance->shape = vm.emptyShape;
  instance->klass = klass;
  instance->frozen = false;
  instance->builtByConstructor = false;
  instance->prototype = NULL;
  instance->privates = NULL;
  instance->attributes = NULL;
  instance->as.slots.values = NULL;
  instance->as.slots.capacity = 0;
  csPopTempRoot();
  return instance;
}

ObjBoundMethod *csBoundMethodNew(Value receiver, Obj *method) {
  if (IS_OBJ(receiver)) csPushTempRoot(AS_OBJ(receiver));
  csPushTempRoot(method);
  ObjBoundMethod *bound = CS_ALLOCATE(ObjBoundMethod, 1);
  csObjectRegister((Obj *)bound, OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  bound->presets = NULL;
  csPopTempRoot();
  if (IS_OBJ(receiver)) csPopTempRoot();
  return bound;
}

ObjClosure *csClassFindMethod(ObjClass *klass, ObjString *name) {
  for (ObjClass *current = klass; current != NULL; current = current->superclass) {
    Value method;
    if (csTableGet(&current->methods, name, &method)) {
      return (ObjClosure *)AS_OBJ(method);
    }
  }
  return NULL;
}

static ObjClosure *findAccessor(ObjClass *klass, ObjString *name, bool isGetter) {
  for (ObjClass *current = klass; current != NULL; current = current->superclass) {
    Value accessor;
    if (csTableGet(isGetter ? &current->getters : &current->setters, name, &accessor)) {
      return (ObjClosure *)AS_OBJ(accessor);
    }
  }
  return NULL;
}

ObjClosure *csClassFindGetter(ObjClass *klass, ObjString *name) {
  return findAccessor(klass, name, true);
}

ObjClosure *csClassFindSetter(ObjClass *klass, ObjString *name) {
  return findAccessor(klass, name, false);
}

bool csClassDescendsFrom(const ObjClass *klass, const ObjClass *other) {
  for (const ObjClass *current = klass; current != NULL; current = current->superclass) {
    if (current == other) return true;
  }
  return false;
}

ObjArray *csArrayNew(void) {
  ObjArray *array = CS_ALLOCATE(ObjArray, 1);
  csObjectRegister((Obj *)array, OBJ_ARRAY);
  csValueArrayInit(&array->elements);
  array->isSpreadMarker = false;
  array->extras = NULL;
  return array;
}

bool csArrayGetExtra(ObjArray *array, ObjString *key, Value *out) {
  if (array->extras == NULL) return false;
  return csTableGet(array->extras, key, out);
}

void csArrayPutExtra(ObjArray *array, const char *name, int length, Value value) {
  csPushTempRoot((Obj *)array);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));

  if (array->extras == NULL) {
    Table *table = CS_ALLOCATE(Table, 1);
    csTableInit(table);
    array->extras = table;
  }
  ObjString *key = csStringCopy(name, length);
  csPushTempRoot((Obj *)key);
  csTableSet(array->extras, key, value);
  csPopTempRoot();

  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}

void csObjectFreeze(ObjObject *object) { object->frozen = true; }
