#include <stdio.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/regex.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

/* FNV-1a. Cheap, well-distributed for short identifier-like keys. */
static uint32_t hashString(const char *key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619u;
  }
  return hash;
}

/* Links a freshly allocated object into the list the collector sweeps. */
static void registerObject(Obj *object, ObjType type) {
  object->type = type;
  object->isMarked = false;
  object->next = vm.objects;
  vm.objects = object;
}

static ObjString *allocateString(const char *chars, int length, uint32_t hash) {
  size_t size = sizeof(ObjString) + (size_t)length + 1;
  ObjString *string = (ObjString *)csReallocate(NULL, 0, size);
  registerObject((Obj *)string, OBJ_STRING);

  string->length = length;
  string->hash = hash;
  memcpy(string->chars, chars, (size_t)length);
  string->chars[length] = '\0';

  /* Interning can allocate, so keep the new string reachable across the insert. */
  csPushTempRoot((Obj *)string);
  csTableSet(&vm.strings, string, NULL_VAL);
  csPopTempRoot();

  return string;
}

ObjString *csStringCopy(const char *chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString *interned = csTableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) return interned;
  return allocateString(chars, length, hash);
}

ObjString *csStringTakeOwnership(char *chars, int length) {
  ObjString *result = csStringCopy(chars, length);
  csReallocate(chars, (size_t)length + 1, 0);
  return result;
}

ObjString *csStringConcat(ObjString *a, ObjString *b) {
  int length = a->length + b->length;
  char *chars = CS_ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, (size_t)a->length);
  memcpy(chars + a->length, b->chars, (size_t)b->length);
  chars[length] = '\0';
  return csStringTakeOwnership(chars, length);
}

ObjNative *csNativeNew(NativeFn function, const char *name, int arity) {
  /* Intern the name first: it allocates, and doing it after the ObjNative is
   * created would leave that object unreachable across a collection. */
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  ObjNative *native = CS_ALLOCATE(ObjNative, 1);
  registerObject((Obj *)native, OBJ_NATIVE);
  native->function = function;
  native->name = nameString;
  native->arity = arity;
  native->statics = NULL;

  csPopTempRoot();
  return native;
}

ObjObject *csObjectNew(const char *name) {
  ObjString *nameString = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)nameString);

  /* Nothing between the allocation and the last field write may allocate: the
   * object is on the sweep list from registerObject onward, so a collection
   * here would walk uninitialised slots. */
  ObjObject *object = CS_ALLOCATE(ObjObject, 1);
  registerObject((Obj *)object, OBJ_OBJECT);
  object->name = nameString;
  object->shape = vm.emptyShape;
  object->klass = NULL;
  object->frozen = false;
  object->as.slots.values = NULL;
  object->as.slots.capacity = 0;

  csPopTempRoot();
  return object;
}

static void ensureSlots(ObjObject *object, int needed) {
  if (object->as.slots.capacity >= needed) return;
  int oldCapacity = object->as.slots.capacity;
  int capacity = oldCapacity < 4 ? 4 : oldCapacity;
  while (capacity < needed) capacity *= 2;
  object->as.slots.values =
      CS_GROW_ARRAY(Value, object->as.slots.values, oldCapacity, capacity);
  object->as.slots.capacity = capacity;
}

/* Moves an object out of shape mode for good. Called once, when it grows past
 * the slot limit; see the comment on CS_SHAPE_MAX_SLOTS for why that limit
 * exists. Every allocation below happens while the object is still a valid
 * shape-mode object, so a collection in the middle is harmless. */
static void convertToDictionary(ObjObject *object) {
  Shape *shape = object->shape;
  int count = shape->slotCount;
  int capacity = count < 8 ? 8 : count;

  Table table;
  csTableInit(&table);
  ObjString **keys = CS_ALLOCATE(ObjString *, capacity);
  for (int i = 0; i < count; i++) {
    keys[i] = shape->keys[i];
    csTableSet(&table, shape->keys[i], object->as.slots.values[i]);
  }

  Value *oldValues = object->as.slots.values;
  int oldCapacity = object->as.slots.capacity;

  object->shape = NULL;
  object->as.dictionary.table = table;
  object->as.dictionary.keys = keys;
  object->as.dictionary.count = count;
  object->as.dictionary.capacity = capacity;

  CS_FREE_ARRAY(Value, oldValues, oldCapacity);
}

static void dictionaryPut(ObjObject *object, ObjString *key, Value value) {
  if (!csTableSet(&object->as.dictionary.table, key, value)) return;

  if (object->as.dictionary.capacity < object->as.dictionary.count + 1) {
    int oldCapacity = object->as.dictionary.capacity;
    object->as.dictionary.capacity = CS_GROW_CAPACITY(oldCapacity);
    object->as.dictionary.keys =
        CS_GROW_ARRAY(ObjString *, object->as.dictionary.keys, oldCapacity,
                      object->as.dictionary.capacity);
  }
  object->as.dictionary.keys[object->as.dictionary.count++] = key;
}

void csObjectPut(ObjObject *object, ObjString *key, Value value) {
  if (object->shape == NULL) {
    csPushTempRoot((Obj *)object);
    if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
    csPushTempRoot((Obj *)key);
    dictionaryPut(object, key, value);
    csPopTempRoot();
    if (IS_OBJ(value)) csPopTempRoot();
    csPopTempRoot();
    return;
  }

  /* Overwriting an existing property is the common case and never allocates,
   * so it is worth answering before any of the rooting below. */
  int slot;
  if (csShapeLookup(object->shape, key, &slot)) {
    object->as.slots.values[slot] = value;
    return;
  }

  csPushTempRoot((Obj *)object);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  csPushTempRoot((Obj *)key);

  Shape *next = csShapeTransition(object->shape, key);
  if (next == NULL) {
    convertToDictionary(object);
    dictionaryPut(object, key, value);
  } else {
    /* The new shape is reachable only through its parent's transition edge,
     * and that edge is weak — so until this object adopts it, a collection
     * would prune the edge and sweep the shape out from under us. Growing the
     * slot array is exactly such a collection point. */
    csPushTempRoot((Obj *)next);

    /* Order matters too. The collector sizes its walk of the slots from the
     * shape, so the object keeps its old shape until the new slot actually
     * holds a value. */
    ensureSlots(object, next->slotCount);
    object->as.slots.values[next->slotCount - 1] = value;
    object->shape = next;

    csPopTempRoot();
  }

  csPopTempRoot();
  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}

bool csObjectGet(ObjObject *object, ObjString *key, Value *out) {
  if (object->shape == NULL) {
    return csTableGet(&object->as.dictionary.table, key, out);
  }
  int slot;
  if (!csShapeLookup(object->shape, key, &slot)) return false;
  if (out != NULL) *out = object->as.slots.values[slot];
  return true;
}

int csObjectCount(const ObjObject *object) {
  return object->shape != NULL ? object->shape->slotCount
                               : object->as.dictionary.count;
}

ObjString *csObjectKeyAt(const ObjObject *object, int index) {
  return object->shape != NULL ? object->shape->keys[index]
                               : object->as.dictionary.keys[index];
}

Value csObjectValueAt(ObjObject *object, int index) {
  if (object->shape != NULL) return object->as.slots.values[index];
  Value value;
  return csTableGet(&object->as.dictionary.table,
                    object->as.dictionary.keys[index], &value)
             ? value
             : UNDEFINED_VAL;
}

ObjFunction *csFunctionNew(void) {
  ObjFunction *function = CS_ALLOCATE(ObjFunction, 1);
  registerObject((Obj *)function, OBJ_FUNCTION);
  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  function->module = NULL;
  function->isAsync = false;
  function->hotness = 0;
  function->jitState = 0; /* JIT_INTERPRETED */
  function->jitCode = NULL;
  function->typedSites = 0;
  function->genericSites = 0;
  csChunkInit(&function->chunk);
  return function;
}

ObjUpvalue *csUpvalueNew(Value *slot) {
  ObjUpvalue *upvalue = CS_ALLOCATE(ObjUpvalue, 1);
  registerObject((Obj *)upvalue, OBJ_UPVALUE);
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
  registerObject((Obj *)closure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
}

ObjModule *csModuleNew(ObjString *path) {
  csPushTempRoot((Obj *)path);
  ObjModule *module = CS_ALLOCATE(ObjModule, 1);
  registerObject((Obj *)module, OBJ_MODULE);
  module->path = path;
  module->body = NULL;
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
  registerObject((Obj *)regex, OBJ_REGEX);
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
  registerObject((Obj *)promise, OBJ_PROMISE);
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
  registerObject((Obj *)fiber, OBJ_FIBER);
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
  return fiber;
}

ObjClass *csClassNew(ObjString *name) {
  csPushTempRoot((Obj *)name);
  ObjClass *klass = CS_ALLOCATE(ObjClass, 1);
  registerObject((Obj *)klass, OBJ_CLASS);
  klass->name = name;
  klass->superclass = NULL;
  klass->initializer = NULL;
  klass->fieldInit = NULL;
  csTableInit(&klass->methods);
  csTableInit(&klass->statics);
  csTableInit(&klass->getters);
  csTableInit(&klass->setters);
  csPopTempRoot();
  return klass;
}

ObjObject *csInstanceNew(ObjClass *klass) {
  csPushTempRoot((Obj *)klass);
  ObjObject *instance = CS_ALLOCATE(ObjObject, 1);
  registerObject((Obj *)instance, OBJ_OBJECT);
  instance->name = klass->name;
  instance->shape = vm.emptyShape;
  instance->klass = klass;
  instance->frozen = false;
  instance->as.slots.values = NULL;
  instance->as.slots.capacity = 0;
  csPopTempRoot();
  return instance;
}

ObjBoundMethod *csBoundMethodNew(Value receiver, Obj *method) {
  if (IS_OBJ(receiver)) csPushTempRoot(AS_OBJ(receiver));
  csPushTempRoot(method);
  ObjBoundMethod *bound = CS_ALLOCATE(ObjBoundMethod, 1);
  registerObject((Obj *)bound, OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
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
  registerObject((Obj *)array, OBJ_ARRAY);
  csValueArrayInit(&array->elements);
  array->isSpreadMarker = false;
  return array;
}

void csObjectFreeze(ObjObject *object) { object->frozen = true; }

void csObjectSetProperty(ObjObject *object, const char *name, Value value) {
  /* Both the interning and the table insert can allocate, so the receiver and
   * the value have to stay rooted for the whole operation. */
  csPushTempRoot((Obj *)object);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));

  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csObjectPut(object, key, value);
  csPopTempRoot();

  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
}

static void printFunctionName(const ObjFunction *function) {
  if (function->name == NULL) {
    printf("[Function: <script>]");
  } else {
    printf("[Function: %s]", function->name->chars);
  }
}

/* A value shown inside a container is quoted if it is a string, so that
 * `[ '1' ]` and `[ 1 ]` are distinguishable. Three places want that. */
static void printNested(Value value) {
  if (IS_STRING(value)) {
    printf("'%s'", AS_CSTRING(value));
  } else {
    csValuePrint(value);
  }
}

void csObjectPrint(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value));
      break;
    case OBJ_NATIVE:
      printf("[Function: %s]", AS_NATIVE(value)->name->chars);
      break;
    case OBJ_OBJECT:
      printf("[Object: %s]", AS_OBJECT(value)->name->chars);
      break;
    case OBJ_CLASS: {
      ObjClass *klass = AS_CLASS(value);
      if (klass->superclass != NULL) {
        printf("[class %s extends %s]", klass->name->chars,
               klass->superclass->name->chars);
      } else {
        printf("[class %s]", klass->name->chars);
      }
      break;
    }
    case OBJ_BOUND_METHOD: {
      Obj *method = AS_BOUND_METHOD(value)->method;
      if (method->type == OBJ_NATIVE) {
        printf("[Function: %s]", ((ObjNative *)method)->name->chars);
      } else {
        printFunctionName(((ObjClosure *)method)->function);
      }
      break;
    }
    case OBJ_MODULE:
      printf("[Module: %s]", AS_MODULE(value)->path->chars);
      break;
    case OBJ_REGEX:
      printf("/%s/%s", AS_REGEX(value)->source->chars, AS_REGEX(value)->flags->chars);
      break;

    case OBJ_MAP: {
      /* `Map(2) { 'a' => 1 }` and `Set(2) { 1, 2 }`, as Node prints them. */
      ObjMap *map = AS_MAP(value);
      printf("%s(%d)", map->isSet ? "Set" : "Map", map->liveCount);
      if (map->liveCount == 0) {
        printf(" {}");
        break;
      }
      printf(" { ");
      bool first = true;
      for (int i = 0; i < map->count; i++) {
        if (!map->entries[i].present) continue;
        if (!first) printf(", ");
        first = false;
        printNested(map->entries[i].key);
        if (!map->isSet) {
          printf(" => ");
          printNested(map->entries[i].value);
        }
      }
      printf(" }");
      break;
    }

    case OBJ_FIBER:
      printf("[internal]");
      break;
    case OBJ_PROMISE: {
      ObjPromise *promise = AS_PROMISE(value);
      if (promise->state == PROMISE_PENDING) {
        printf("Promise { <pending> }");
      } else {
        printf("Promise { ");
        if (promise->state == PROMISE_REJECTED) printf("<rejected> ");
        /* Quoted, the way a value nested inside a container is printed. */
        if (IS_STRING(promise->value)) {
          printf("'%s'", AS_CSTRING(promise->value));
        } else {
          csValuePrint(promise->value);
        }
        printf(" }");
      }
      break;
    }
    case OBJ_FUNCTION:
      printFunctionName((ObjFunction *)AS_OBJ(value));
      break;
    case OBJ_CLOSURE:
      printFunctionName(AS_CLOSURE(value)->function);
      break;
    case OBJ_UPVALUE:
    case OBJ_SHAPE:
      /* Never reachable from user code; only the collector sees these. */
      printf("[internal]");
      break;

    case OBJ_ARRAY: {
      ObjArray *array = AS_ARRAY(value);
      printf("[ ");
      for (int i = 0; i < array->elements.count; i++) {
        if (i > 0) printf(", ");
        /* Strings are quoted inside a container, the way console.log does it,
         * so `[ "1" ]` and `[ 1 ]` are distinguishable. */
        if (IS_STRING(array->elements.values[i])) {
          printf("'%s'", AS_CSTRING(array->elements.values[i]));
        } else {
          csValuePrint(array->elements.values[i]);
        }
      }
      printf(" ]");
      break;
    }
  }
}

void csObjectBlacken(Obj *object) {
  switch (object->type) {
    case OBJ_STRING:
      break; /* no outgoing references */

    case OBJ_NATIVE: {
      ObjNative *native = (ObjNative *)object;
      csMarkObject((Obj *)native->name);
      csMarkObject((Obj *)native->statics);
      break;
    }

    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      csMarkObject((Obj *)instance->name);
      csMarkObject((Obj *)instance->klass);
      if (instance->shape != NULL) {
        /* The shape is the authority on how many slots hold a value. Anything
         * beyond slotCount is capacity the object has not grown into yet. */
        csMarkObject((Obj *)instance->shape);
        for (int i = 0; i < instance->shape->slotCount; i++) {
          csMarkValue(instance->as.slots.values[i]);
        }
      } else {
        csTableMark(&instance->as.dictionary.table);
        for (int i = 0; i < instance->as.dictionary.count; i++) {
          csMarkObject((Obj *)instance->as.dictionary.keys[i]);
        }
      }
      break;
    }

    case OBJ_SHAPE:
      csShapeBlacken((Shape *)object);
      break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csMarkObject((Obj *)klass->name);
      csMarkObject((Obj *)klass->superclass);
      csMarkObject((Obj *)klass->initializer);
      csMarkObject((Obj *)klass->fieldInit);
      csTableMark(&klass->methods);
      csTableMark(&klass->statics);
      csTableMark(&klass->getters);
      csTableMark(&klass->setters);
      break;
    }

    case OBJ_BOUND_METHOD: {
      ObjBoundMethod *bound = (ObjBoundMethod *)object;
      csMarkValue(bound->receiver);
      csMarkObject(bound->method);
      break;
    }

    case OBJ_REGEX: {
      ObjRegex *regex = (ObjRegex *)object;
      csMarkObject((Obj *)regex->source);
      csMarkObject((Obj *)regex->flags);
      break;
    }

    case OBJ_MAP: {
      ObjMap *map = (ObjMap *)object;
      /* Only the live entries: a tombstone's key and value were cleared when
       * it was deleted, so there is nothing there to keep alive. */
      for (int i = 0; i < map->count; i++) {
        if (!map->entries[i].present) continue;
        csMarkValue(map->entries[i].key);
        csMarkValue(map->entries[i].value);
      }
      break;
    }

    case OBJ_FIBER: {
      ObjFiber *fiber = (ObjFiber *)object;
      /* Only up to stackTop: everything above is the slots it has not grown
       * into yet, and marking those would walk stale values. */
      for (Value *slot = fiber->stack; slot < fiber->stackTop; slot++) {
        csMarkValue(*slot);
      }
      for (int i = 0; i < fiber->frameCount; i++) {
        csMarkObject((Obj *)fiber->frames[i].closure);
      }
      for (ObjUpvalue *upvalue = fiber->openUpvalues; upvalue != NULL;
           upvalue = upvalue->next) {
        csMarkObject((Obj *)upvalue);
      }
      csMarkObject((Obj *)fiber->promise);
      break;
    }

    case OBJ_PROMISE: {
      ObjPromise *promise = (ObjPromise *)object;
      csMarkValue(promise->value);
      for (int i = 0; i < promise->reactionCount; i++) {
        csMarkValue(promise->reactions[i].onFulfilled);
        csMarkValue(promise->reactions[i].onRejected);
        csMarkObject((Obj *)promise->reactions[i].result);
        csMarkObject((Obj *)promise->reactions[i].combineState);
        csMarkObject((Obj *)promise->reactions[i].fiber);
      }
      break;
    }

    case OBJ_MODULE: {
      ObjModule *module = (ObjModule *)object;
      csMarkObject((Obj *)module->path);
      csMarkObject((Obj *)module->body);
      csTableMark(&module->globals);
      csTableMark(&module->globalConsts);
      csTableMark(&module->exports);
      break;
    }

    case OBJ_FUNCTION: {
      /* A function owns its constant pool, so every literal in its body is
       * live for as long as the function is. */
      ObjFunction *function = (ObjFunction *)object;
      csMarkObject((Obj *)function->name);
      csMarkObject((Obj *)function->module);
      for (int i = 0; i < function->chunk.constants.count; i++) {
        csMarkValue(function->chunk.constants.values[i]);
      }
      break;
    }

    case OBJ_UPVALUE:
      /* `closed` holds the value once the variable has left the stack. While
       * the upvalue is still open it is empty, and the stack root covers it. */
      csMarkValue(((ObjUpvalue *)object)->closed);
      break;

    case OBJ_CLOSURE: {
      ObjClosure *closure = (ObjClosure *)object;
      csMarkObject((Obj *)closure->function);
      for (int i = 0; i < closure->upvalueCount; i++) {
        csMarkObject((Obj *)closure->upvalues[i]);
      }
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      for (int i = 0; i < array->elements.count; i++) {
        csMarkValue(array->elements.values[i]);
      }
      break;
    }
  }
}

void csObjectFree(Obj *object) {
  switch (object->type) {
    case OBJ_STRING: {
      ObjString *string = (ObjString *)object;
      csReallocate(object, sizeof(ObjString) + (size_t)string->length + 1, 0);
      break;
    }
    case OBJ_NATIVE:
      CS_FREE(ObjNative, object);
      break;
    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      if (instance->shape != NULL) {
        CS_FREE_ARRAY(Value, instance->as.slots.values, instance->as.slots.capacity);
      } else {
        csTableFree(&instance->as.dictionary.table);
        CS_FREE_ARRAY(ObjString *, instance->as.dictionary.keys,
                      instance->as.dictionary.capacity);
      }
      CS_FREE(ObjObject, object);
      break;
    }

    case OBJ_SHAPE:
      csShapeFree((Shape *)object);
      break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csTableFree(&klass->methods);
      csTableFree(&klass->statics);
      csTableFree(&klass->getters);
      csTableFree(&klass->setters);
      CS_FREE(ObjClass, object);
      break;
    }

    case OBJ_BOUND_METHOD:
      /* The receiver and the method both belong to whoever else holds them. */
      CS_FREE(ObjBoundMethod, object);
      break;

    case OBJ_REGEX: {
      ObjRegex *regex = (ObjRegex *)object;
      csRegexFree(regex->program);
      CS_FREE(ObjRegex, object);
      break;
    }

    case OBJ_MAP: {
      ObjMap *map = (ObjMap *)object;
      CS_FREE_ARRAY(MapEntry, map->entries, map->capacity);
      CS_FREE_ARRAY(int, map->index, map->indexCapacity);
      CS_FREE(ObjMap, object);
      break;
    }

    case OBJ_FIBER: {
      ObjFiber *fiber = (ObjFiber *)object;
      CS_FREE_ARRAY(Value, fiber->stack, fiber->stackCapacity);
      CS_FREE_ARRAY(CallFrame, fiber->frames, CS_FIBER_FRAMES);
      CS_FREE_ARRAY(ExceptionHandler, fiber->handlers, CS_FIBER_HANDLERS);
      CS_FREE(ObjFiber, object);
      break;
    }

    case OBJ_PROMISE: {
      ObjPromise *promise = (ObjPromise *)object;
      CS_FREE_ARRAY(Reaction, promise->reactions, promise->reactionCapacity);
      CS_FREE(ObjPromise, object);
      break;
    }

    case OBJ_MODULE: {
      ObjModule *module = (ObjModule *)object;
      csTableFree(&module->globals);
      csTableFree(&module->globalConsts);
      csTableFree(&module->exports);
      CS_FREE(ObjModule, object);
      break;
    }

    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *)object;
      csChunkFree(&function->chunk);
      CS_FREE(ObjFunction, object);
      break;
    }

    case OBJ_UPVALUE:
      /* The captured value belongs to whoever else still references it. */
      CS_FREE(ObjUpvalue, object);
      break;

    case OBJ_CLOSURE: {
      /* The function is shared between closures, so only the array goes. */
      ObjClosure *closure = (ObjClosure *)object;
      CS_FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCount);
      CS_FREE(ObjClosure, object);
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      csValueArrayFree(&array->elements);
      CS_FREE(ObjArray, object);
      break;
    }
  }
}
