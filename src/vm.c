#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/debug.h"
#include "cscript/jit.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/parser.h"
#include "cscript/typecheck.h"
#include "cscript/vm.h"
#include "vm_internal.h"

VM vm;

void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.handlerCount = 0;
  vm.hasPendingException = false;
  vm.openUpvalues = NULL;
  vm.tempRootCount = 0;
}

/* The main program's execution state. A fiber for `await` gets its own arrays
 * on the heap; this one is static because it exists for the whole run. */
static Value mainStack[CS_STACK_MAX];
static CallFrame mainFrames[CS_FRAMES_MAX];
static ExceptionHandler mainHandlers[CS_HANDLERS_MAX];

void csVMInit(void) {
  vm.stack = mainStack;
  vm.stackCapacity = CS_STACK_MAX;
  vm.frames = mainFrames;
  vm.frameCapacity = CS_FRAMES_MAX;
  vm.handlers = mainHandlers;
  vm.handlerCapacity = CS_HANDLERS_MAX;
  resetStack();
  vm.objects = NULL;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;
  vm.sourceName = "<script>";

  csTableInit(&vm.builtins);
  csTableInit(&vm.builtinConsts);
  csTableInit(&vm.modules);
  vm.mainModule = NULL;
  vm.currentFiber = NULL;
  vm.fiberSuspended = false;
  vm.deferUncaught = false;
  vm.pendingCount = 0;
  csTableInit(&vm.strings);
  csTableInit(&vm.arrayMethods);
  csTableInit(&vm.stringMethods);
  csTableInit(&vm.promiseMethods);

  vm.microtasks = NULL;
  vm.microtaskCount = 0;
  vm.microtaskCapacity = 0;
  vm.microtaskHead = 0;
  vm.timerCount = 0;
  vm.nextTimerId = 1;
  vm.timerSequence = 0;
  vm.rejected = NULL;
  vm.rejectedCount = 0;
  vm.rejectedCapacity = 0;

  /* Before any object exists: csObjectNew reads it, and the collector marks
   * it. */
  vm.emptyShape = NULL;
  vm.absentShape = NULL;
  vm.emptyShape = csShapeNewRoot();
  vm.absentShape = csShapeNewRoot();

  csNativesInstall();

  /* Code with no file of its own — `-e`, the REPL, a string handed to
   * csInterpret — still needs a scope to live in. */
  vm.mainModule = csModuleNew(csStringCopy("<main>", 6));
}

void csVMMarkBuiltinConst(ObjString *name) {
  csTableSet(&vm.builtinConsts, name, BOOL_VAL(true));
}

void csVMFree(void) {
  CS_FREE_ARRAY(Microtask, vm.microtasks, vm.microtaskCapacity);
  CS_FREE_ARRAY(ObjPromise *, vm.rejected, vm.rejectedCapacity);
  csTableFree(&vm.promiseMethods);
  csTableFree(&vm.builtins);
  csTableFree(&vm.builtinConsts);
  csTableFree(&vm.modules);
  csTableFree(&vm.arrayMethods);
  csTableFree(&vm.stringMethods);
  csTableFree(&vm.strings);
  csFreeAllObjects();
}

void csVMPush(Value value) {
  if (vm.stackTop - vm.stack >= vm.stackCapacity) {
    fprintf(stderr, "cscript: value stack overflow\n");
    resetStack();
    return;
  }
  *vm.stackTop++ = value;
}

Value csVMPop(void) { return *(--vm.stackTop); }

static Value peekStack(int distance) { return vm.stackTop[-1 - distance]; }

Value csVMPeek(int distance) { return peekStack(distance); }

/* True when a double holds an exact integer small enough to move into an
 * int64_t without loss. 2^53 is the largest such value for a double, and it is
 * far inside int64_t's range, so the conversion below cannot overflow. */
static inline bool isExactInteger(double value) {
  return value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
         value == (double)(int64_t)value;
}

/* How a file is named in a stack trace: relative to where the program was
 * started when it sits underneath, absolute otherwise. Traces cross module
 * boundaries now, so each frame names its own file rather than sharing one. */
static const char *frameSourceName(const CallFrame *frame) {
  ObjModule *module = frame->closure->function->module;
  if (module == NULL || module == vm.mainModule) return vm.sourceName;
  return csModuleDisplayPath(module->path->chars);
}

void csVMRuntimeError(const char *format, ...) {
  /* stdout is block-buffered to a pipe and stderr is not, so without this the
   * error would jump ahead of output the program already produced. */
  fflush(stdout);

  va_list args;
  va_start(args, format);
  fprintf(stderr, "cscript: runtime error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  /* Walk the frames innermost-first so the trace reads like a call stack. */
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    /* `ip` has already advanced past the failing instruction. */
    size_t instruction = (size_t)(frame->ip - function->chunk.code - 1);
    int line = function->chunk.lines[instruction];

    const char *source = frameSourceName(frame);
    if (function->name == NULL) {
      fprintf(stderr, "  at %s:%d\n", source, line);
    } else {
      fprintf(stderr, "  at %s (%s:%d)\n", function->name->chars, source, line);
    }
  }

  resetStack();
}

/* JS `+`: if either side is a string, both are stringified and concatenated;
 * otherwise both are coerced to numbers and added. */
static bool concatenateOrAdd(void) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (IS_STRING(a) || IS_STRING(b)) {
    size_t aLength = 0;
    size_t bLength = 0;
    char *aText = csValueToCString(a, &aLength);
    char *bText = csValueToCString(b, &bLength);
    if (aText == NULL || bText == NULL) {
      free(aText);
      free(bText);
      csVMRuntimeError("out of memory while concatenating strings");
      return false;
    }

    size_t length = aLength + bLength;
    char *joined = CS_ALLOCATE(char, length + 1);
    memcpy(joined, aText, aLength);
    memcpy(joined + aLength, bText, bLength);
    joined[length] = '\0';
    free(aText);
    free(bText);

    ObjString *result = csStringTakeOwnership(joined, (int)length);
    csVMPop();
    csVMPop();
    csVMPush(OBJ_VAL(result));
    return true;
  }

  if (!IS_NUMBER(a) || !IS_NUMBER(b)) {
    csVMRuntimeError("cannot add %s and %s", csValueTypeName(a), csValueTypeName(b));
    return false;
  }

  csVMPop();
  csVMPop();
  csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
  return true;
}

/* Pushes a frame for a user function. The frame's window starts at the callee
 * itself, so slot 0 is the function and slots 1..arity are the arguments —
 * exactly where the compiler assigned the parameters. */
bool callClosure(ObjClosure *closure, int argCount) {
  ObjFunction *function = closure->function;

  if (argCount != function->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d",
                     function->name != NULL ? function->name->chars : "<anonymous>",
                     function->arity, function->arity == 1 ? "" : "s", argCount);
    return false;
  }

  if (vm.frameCount == vm.frameCapacity) {
    csVMRuntimeError("call stack overflow (limit %d frames)", vm.frameCapacity);
    return false;
  }

  CS_JIT_TICK(function);

  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

/* `receiver` is the value a method was invoked on, or undefined for a plain
 * call. It sits on the stack below the arguments either way, so dropping
 * `argCount + 1` slots discards it along with them. */
bool callNative(ObjNative *native, Value receiver, int argCount) {
  if (native->arity >= 0 && argCount != native->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d", native->name->chars,
                     native->arity, native->arity == 1 ? "" : "s", argCount);
    return false;
  }

  Value result;
  if (!native->function(receiver, argCount, vm.stackTop - argCount, &result)) {
    return false;
  }

  /* Drop the arguments and the callee, then push the result in its place. */
  vm.stackTop -= argCount + 1;
  csVMPush(result);
  return true;
}


bool callValue(Value callee, int argCount) {
  if (IS_CLOSURE(callee)) {
    ObjClosure *closure = AS_CLOSURE(callee);
    if (closure->function->isAsync) return callAsyncFunction(closure, argCount);
    return callClosure(closure, argCount);
  }
  if (IS_NATIVE(callee)) return callNative(AS_NATIVE(callee), UNDEFINED_VAL, argCount);

  if (IS_BOUND_METHOD(callee)) {
    ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
    if (bound->method->type == OBJ_NATIVE) {
      /* Natives already take a receiver, so a bound one is simply called with
       * the value it was bound to. */
      return callNative((ObjNative *)bound->method, bound->receiver, argCount);
    }
    /* The receiver goes back into slot 0, which is where the method's `this`
     * reads from — the same place OP_INVOKE would have left it. */
    vm.stackTop[-argCount - 1] = bound->receiver;
    return callClosure((ObjClosure *)bound->method, argCount);
  }

  if (IS_CLASS(callee)) {
    csVMRuntimeError("%s is a class; write 'new %s(...)' to construct one",
                     AS_CLASS(callee)->name->chars, AS_CLASS(callee)->name->chars);
    return false;
  }

  csVMRuntimeError("%s is not a function", csValueTypeName(callee));
  return false;
}

/* Calls a class method, leaving the receiver in slot 0 so `this` resolves to
 * it. Every other call path overwrites that slot with the callee, which is
 * harmless because nothing else names it — a method is the one case where the
 * body reads it. */
/* `super.name` in a static method means the superclass's static of that name,
 * and in an instance method its instance method. Nothing in the bytecode says
 * which, but the receiver does: a static is called with the class itself. */
static ObjClosure *findSuperMember(ObjClass *superclass, ObjString *name,
                                   bool isStatic) {
  if (!isStatic) return csClassFindMethod(superclass, name);
  for (ObjClass *klass = superclass; klass != NULL; klass = klass->superclass) {
    Value member;
    if (csTableGet(&klass->statics, name, &member)) {
      return (ObjClosure *)AS_OBJ(member);
    }
  }
  return NULL;
}

static bool callMethod(ObjClosure *method, int argCount) {
  /* An async method is still an async call: it needs its own fiber, and the
   * receiver already sits where that fiber's slot 0 will be. */
  if (method->function->isAsync) return callAsyncFunction(method, argCount);
  return callClosure(method, argCount);
}

/* The method table a receiver's built-ins live in, or NULL when it has none. */
static Table *methodTableFor(Value receiver) {
  if (IS_ARRAY(receiver)) return &vm.arrayMethods;
  if (IS_STRING(receiver)) return &vm.stringMethods;
  if (IS_PROMISE(receiver)) return &vm.promiseMethods;
  return NULL;
}

/* Looks `name` up on `receiver` and calls it, leaving the result where the
 * receiver was. Returns false with an error already reported.
 *
 * A plain object's own properties win over anything else, because a user
 * function stored on an object is exactly what it looks like. */
static bool invokeMethod(Value receiver, ObjString *name, int argCount) {
  if (IS_OBJECT(receiver)) {
    Value method;
    if (csObjectGet(AS_OBJECT(receiver), name, &method)) {
      /* Overwrite the receiver slot so the callee sits directly below its
       * arguments, which is the shape callValue expects. */
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }

    /* Only now the class chain, so a field holding a function still wins —
     * which also means `this` is bound for methods declared in a class body
     * and not for a function that merely lives on an object. */
    ObjObject *instance = AS_OBJECT(receiver);
    if (instance->klass != NULL) {
      ObjClosure *classMethod = csClassFindMethod(instance->klass, name);
      if (classMethod != NULL) return callMethod(classMethod, argCount);
    }

    csVMRuntimeError("'%s' has no method '%s'", instance->name->chars, name->chars);
    return false;
  }

  if (IS_CLASS(receiver)) {
    /* A static keeps the class in slot 0, so `this` inside one is the class. */
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value method;
      if (csTableGet(&klass->statics, name, &method)) {
        return callMethod((ObjClosure *)AS_OBJ(method), argCount);
      }
    }
    csVMRuntimeError("class %s has no static method '%s'",
                     AS_CLASS(receiver)->name->chars, name->chars);
    return false;
  }

  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    Value method;
    if (csObjectGet(AS_NATIVE(receiver)->statics, name, &method)) {
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }
    csVMRuntimeError("'%s' has no method '%s'", AS_NATIVE(receiver)->name->chars,
                     name->chars);
    return false;
  }

  Table *methods = methodTableFor(receiver);
  if (methods != NULL) {
    Value method;
    if (csTableGet(methods, name, &method)) {
      return callNative(AS_NATIVE(method), receiver, argCount);
    }
    /* typeof [] is "object", which would make the message confusing here. */
    csVMRuntimeError("%s has no method '%s'",
                     IS_ARRAY(receiver)     ? "array"
                     : IS_PROMISE(receiver) ? "a promise"
                                            : csValueTypeName(receiver),
                     name->chars);
    return false;
  }

  csVMRuntimeError("cannot call '%s' on %s", name->chars, csValueTypeName(receiver));
  return false;
}

/* Finds or creates the upvalue for a stack slot.
 *
 * The open list is kept sorted by descending slot address, so two closures
 * capturing the same variable share one upvalue — which is what makes writes
 * through one visible to the other. */
static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *previous = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;

  while (upvalue != NULL && upvalue->location > local) {
    previous = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) return upvalue;

  ObjUpvalue *created = csUpvalueNew(local);
  created->next = upvalue;
  if (previous == NULL) {
    vm.openUpvalues = created;
  } else {
    previous->next = created;
  }
  return created;
}

/* Moves every upvalue at or above `last` off the stack and onto its own heap
 * cell, so closures keep working once the frame is gone. */
static void closeUpvalues(Value *last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

#ifdef CS_DEBUG_PROFILE_OPCODES
/* Counts how often each opcode follows each other opcode, so superinstruction
 * candidates are chosen from data rather than from intuition. */
static uint64_t opcodePairs[OP_COUNT][OP_COUNT];
static uint64_t opcodeCounts[OP_COUNT];
static int previousOpcode = -1;

static void recordOpcode(uint8_t opcode) {
  opcodeCounts[opcode]++;
  if (previousOpcode >= 0) opcodePairs[previousOpcode][opcode]++;
  previousOpcode = opcode;
}

void csVMDumpOpcodeProfile(void);

void csVMDumpOpcodeProfile(void) {
  uint64_t total = 0;
  for (int i = 0; i < OP_COUNT; i++) total += opcodeCounts[i];
  if (total == 0) return;

  fprintf(stderr, "\n== opcode profile: %llu instructions ==\n",
          (unsigned long long)total);

  /* Top single opcodes. */
  fprintf(stderr, "\n-- most executed --\n");
  for (int rank = 0; rank < 10; rank++) {
    int best = -1;
    for (int i = 0; i < OP_COUNT; i++) {
      if (opcodeCounts[i] > 0 && (best < 0 || opcodeCounts[i] > opcodeCounts[best])) {
        best = i;
      }
    }
    if (best < 0) break;
    fprintf(stderr, "  %-22s %10llu  %5.1f%%\n", csOpcodeName((OpCode)best),
            (unsigned long long)opcodeCounts[best],
            100.0 * (double)opcodeCounts[best] / (double)total);
    opcodeCounts[best] = 0;
  }

  /* Top adjacent pairs — each one is a superinstruction candidate. */
  fprintf(stderr, "\n-- most frequent pairs --\n");
  for (int rank = 0; rank < 15; rank++) {
    int bestA = -1, bestB = -1;
    uint64_t best = 0;
    for (int a = 0; a < OP_COUNT; a++) {
      for (int b = 0; b < OP_COUNT; b++) {
        if (opcodePairs[a][b] > best) {
          best = opcodePairs[a][b];
          bestA = a;
          bestB = b;
        }
      }
    }
    if (bestA < 0) break;
    fprintf(stderr, "  %-22s -> %-22s %9llu  %5.1f%%\n", csOpcodeName((OpCode)bestA),
            csOpcodeName((OpCode)bestB), (unsigned long long)best,
            100.0 * (double)best / (double)total);
    opcodePairs[bestA][bestB] = 0;
  }
  fprintf(stderr, "\n");
}
#endif

/* Dispatch strategy.
 *
 * A switch compiles to one indirect branch shared by every opcode, so the CPU's
 * branch predictor has a single history slot for the whole interpreter and
 * mispredicts constantly. Computed goto gives each opcode its own dispatch
 * branch at the end of its own handler, so the predictor can learn the pairs
 * that actually follow one another — OP_GET_LOCAL is nearly always followed by
 * OP_CONSTANT in a loop, and that becomes predictable.
 *
 * The switch version is kept for compilers without the labels-as-values
 * extension, and both paths run the same handler bodies.
 */
/* Define CS_NO_COMPUTED_GOTO to force the portable switch path — `make
 * test-switch` builds that way, so the fallback cannot quietly rot. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(CS_NO_COMPUTED_GOTO)
#define CS_COMPUTED_GOTO 1
#endif

/* Labels-as-values is a GNU extension, which -Wpedantic flags. The use is
 * deliberate and guarded by the feature test above, so the warning is silenced
 * here rather than dropped from the project-wide warning set. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/* Executes until the frame stack unwinds back to `baseFrame`.
 *
 * The top level passes 0, so the loop ends when the script returns. A native
 * calling back into user code passes the depth it started at, which turns this
 * into a nested interpreter that returns control as soon as that one call is
 * finished — without which `map`, `filter` and a `sort` comparator could not
 * run user code at all. */
/* What a throw could do at this level of the interpreter. */

/* Unwinds to the innermost handler, if that handler belongs to this loop.
 *
 * `baseFrame` is where this interpreter loop started. A handler installed below
 * it was installed by an outer loop, whose C frame is still on the stack under
 * this one — so the throw has to travel out through the return path rather than
 * jump there directly. */
ThrowResult performThrow(Value thrown, int baseFrame, CallFrame **frame) {
  if (vm.handlerCount == 0) return THROW_UNCAUGHT;

  ExceptionHandler *handler = &vm.handlers[vm.handlerCount - 1];
  /* This loop is entered with frameCount == baseFrame + 1, so a handler
   * recorded at baseFrame or below was installed before it started and belongs
   * to an outer loop. Jumping there would resume outer code with this loop's C
   * frame — and the native that started it — still on the stack. */
  if (handler->frameCount <= baseFrame) {
    vm.pendingException = thrown;
    vm.hasPendingException = true;
    return THROW_PROPAGATE;
  }

  vm.handlerCount--;

  /* Discard every frame the throw escaped, closing anything their locals were
   * captured into before the slots are reused. */
  while (vm.frameCount > handler->frameCount) {
    vm.frameCount--;
    closeUpvalues(vm.frames[vm.frameCount].slots);
  }
  closeUpvalues(handler->stackTop);

  vm.stackTop = handler->stackTop;
  *frame = &vm.frames[vm.frameCount - 1];
  (*frame)->ip = handler->ip;

  /* The catch block expects the thrown value where its binding lives. */
  csVMPush(thrown);
  return THROW_HANDLED;
}

static void reportUncaught(Value thrown) {
  size_t length = 0;
  char *text = csValueInspect(thrown, &length);
  csVMRuntimeError("uncaught %s", text != NULL ? text : "value");
  free(text);
}

/* Runs the field initialisers of every class from `klass` up to but not
 * including `stopAt`, base-most first.
 *
 * Only classes that declare no constructor of their own get here: a class with
 * a constructor has its fields compiled into it, at the top of the body for a
 * base class and immediately after `super(...)` for a derived one. That is
 * where JavaScript runs them, and running them anywhere else is observable —
 * it changes the order of `Object.keys`. */
bool runFieldInitializers(ObjClass *klass, ObjClass *stopAt, Value instance) {
  ObjClass *chain[CS_FRAMES_MAX];
  int depth = 0;
  for (ObjClass *current = klass; current != stopAt; current = current->superclass) {
    if (depth == CS_FRAMES_MAX) {
      csVMRuntimeError("class hierarchy is too deep (limit %d)", CS_FRAMES_MAX);
      return false;
    }
    chain[depth++] = current;
  }

  for (int i = depth - 1; i >= 0; i--) {
    if (chain[i]->fieldInit == NULL) continue;
    /* Pushed where the callee would go, so the initialiser's slot 0 — its
     * `this` — is the instance. */
    csVMPush(instance);
    Value ignored;
    if (!csVMCallCallback(OBJ_VAL(chain[i]->fieldInit), 0, &ignored)) return false;
  }
  return true;
}

/* The nearest class at or above `klass` that declares a constructor. A class
 * without one is built by its parent's, which is what makes
 * `class Dog extends Animal {}` still accept the arguments Animal declares. */
static ObjClass *findConstructorOwner(ObjClass *klass) {
  for (ObjClass *current = klass; current != NULL; current = current->superclass) {
    if (current->initializer != NULL) return current;
  }
  return NULL;
}

/* The nearest constructor at or above `klass`, or NULL. */
ObjClosure *findConstructor(ObjClass *klass) {
  ObjClass *owner = findConstructorOwner(klass);
  return owner != NULL ? owner->initializer : NULL;
}


/* The scope a frame's global reads and writes resolve in. Every function in a
 * file carries the module it was compiled in. */
static ObjModule *frameModule(const CallFrame *frame) {
  return frame->closure->function->module;
}

/* The uncached path for assigning to a global: check that the name exists and
 * is not a constant, then record where it lives so the site never has to ask
 * again.
 *
 * Filling the cache is what licenses the fast path to skip the constant check.
 * A name's constness is fixed when it is declared, and a site cannot run
 * before the declaration it refers to — reaching one would have errored here
 * with "is not defined" and left the cache empty. */
static bool assignGlobal(ObjModule *module, ObjString *name, GlobalCache *cache,
                         Value value) {
  if (csTableGet(&module->globalConsts, name, NULL)) {
    csVMRuntimeError("'%s' is a constant and cannot be reassigned", name->chars);
    return false;
  }

  /* Assigning to a name that does not exist would silently create a global in
   * JavaScript; here it is the typo it almost always is. */
  Entry *entry = csTableFindEntry(&module->globals, name);
  if (entry == NULL) {
    csVMRuntimeError("'%s' is not defined", name->chars);
    return false;
  }

  entry->value = value;
  cache->table = &module->globals;
  cache->entry = entry;
  cache->version = module->globals.version;
  cache->filled = true;
  return true;
}

/* What happens to a throw that escapes every handler.
 *
 * Inside an async function it is not an error yet: the call already handed its
 * caller a promise, so the escaping value becomes that promise's rejection and
 * whoever awaits it decides what to do. Anywhere else there is no one left to
 * tell, and it is reported. */
static InterpretResult uncaught(Value thrown) {
  if (vm.currentFiber != NULL || vm.deferUncaught) {
    vm.pendingException = thrown;
    vm.hasPendingException = true;
    return CS_RUNTIME_ERROR;
  }
  reportUncaught(thrown);
  return CS_RUNTIME_ERROR;
}

/* The part of a property read that is not a cache hit. Kept out of line so
 * each of the opcodes that reads a property can have its own tight handler:
 * sharing one body between them costs more than the instruction it saves,
 * because the dispatch table's indirect jump is predicted per opcode. */
static bool propertyReadSlow(ObjString *name, PropertyCache *cache, Value receiver,
                             Value *out) {
  if (IS_OBJECT(receiver)) {
    ObjObject *object = AS_OBJECT(receiver);

    int slot;
    if (object->shape != NULL && csShapeLookup(object->shape, name, &slot)) {
      cache->shape = object->shape;
      cache->slot = slot;
      *out = object->as.slots.values[slot];
      return true;
    }

    /* Dictionary mode, an accessor, an inherited method, or a genuinely absent
     * property. Reading a missing property gives undefined, as in JavaScript —
     * checking for absence is too common to make it an error. */
    if (csObjectGet(object, name, out)) return true;

    if (object->klass != NULL) {
      ObjClosure *getter = csClassFindGetter(object->klass, name);
      if (getter != NULL) {
        /* A getter is a call, so it runs a nested loop. It never enters the
         * shape, which is why the inline caches never see one. */
        csVMPush(receiver);
        return csVMCallCallback(OBJ_VAL(getter), 0, out);
      }

      ObjClosure *method = csClassFindMethod(object->klass, name);
      if (method != NULL) {
        /* Reading a method without calling it is the one case that has to
         * allocate, because the receiver has to travel with it. */
        *out = OBJ_VAL(csBoundMethodNew(receiver, (Obj *)method));
        return true;
      }
    }

    *out = UNDEFINED_VAL;
    return true;
  }

  /* `length` is intrinsic rather than a stored property, so arrays and strings
   * answer it without carrying a table. */
  if (name->length == 6 && memcmp(name->chars, "length", 6) == 0) {
    if (IS_ARRAY(receiver)) {
      *out = NUMBER_VAL(AS_ARRAY(receiver)->elements.count);
      return true;
    }
    if (IS_STRING(receiver)) {
      *out = NUMBER_VAL(AS_STRING(receiver)->length);
      return true;
    }
  }

  /* A callable may carry statics, which is how Number(x) and Number.isInteger
   * coexist. */
  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    Value statik;
    *out = csObjectGet(AS_NATIVE(receiver)->statics, name, &statik) ? statik
                                                                   : UNDEFINED_VAL;
    return true;
  }

  /* A class carries its statics, and inherits its parent's. */
  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value statik;
      if (csTableGet(&klass->statics, name, &statik)) {
        *out = statik;
        return true;
      }
    }
    *out = UNDEFINED_VAL;
    return true;
  }

  csVMRuntimeError("cannot read property '%s' of %s", name->chars,
                   csValueTypeName(receiver));
  return false;
}

/* The part of a property write that is not a cache hit. */
static bool propertyWriteSlow(ObjString *name, PropertyCache *cache, Value receiver,
                              Value value) {
  /* A class carries its statics, so assigning to one is how a counter kept on
   * the class is updated. It never enters a shape, so it never caches. */
  if (IS_CLASS(receiver)) {
    csTableSet(&AS_CLASS(receiver)->statics, name, value);
    return true;
  }

  if (!IS_OBJECT(receiver)) {
    csVMRuntimeError("cannot set property '%s' of %s", name->chars,
                     csValueTypeName(receiver));
    return false;
  }
  if (AS_OBJECT(receiver)->frozen) {
    csVMRuntimeError("'%s.%s' is a built-in and cannot be replaced",
                     AS_OBJECT(receiver)->name->chars, name->chars);
    return false;
  }

  /* A setter takes the write instead of the object storing it. Checked before
   * the store, so the property never enters the shape and the caches stay out
   * of it. */
  if (AS_OBJECT(receiver)->klass != NULL) {
    ObjClosure *setter = csClassFindSetter(AS_OBJECT(receiver)->klass, name);
    if (setter != NULL) {
      csVMPush(receiver);
      csVMPush(value);
      Value ignored;
      return csVMCallCallback(OBJ_VAL(setter), 1, &ignored);
    }
    /* A getter with no setter is read-only, and silently dropping the write is
     * how a bug hides. */
    if (csClassFindGetter(AS_OBJECT(receiver)->klass, name) != NULL) {
      csVMRuntimeError("'%s' has only a getter and cannot be assigned to",
                       name->chars);
      return false;
    }
  }

  csObjectPut(AS_OBJECT(receiver), name, value);

  /* Cache the resulting layout, not the one on the way in. A site that keeps
   * adding fresh properties fills the cache with a shape it has already left,
   * but a site that assigns to the same field of many like-shaped objects —
   * the common one — hits from then on. */
  ObjObject *target = AS_OBJECT(receiver);
  int slot;
  if (target->shape != NULL && csShapeLookup(target->shape, name, &slot)) {
    cache->shape = target->shape;
    cache->slot = slot;
  }
  return true;
}

InterpretResult run(int baseFrame) {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

/* `frame` is cached in a local rather than re-read from vm.frames each time;
 * it is refreshed on every call and return. */
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() \
  (frame->closure->function->chunk.constants.values[READ_SHORT()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define READ_PROPERTY_CACHE() \
  (&frame->closure->function->chunk.propertyCaches[READ_SHORT()])
#define READ_GLOBAL_CACHE() \
  (&frame->closure->function->chunk.globalCaches[READ_SHORT()])

/* Arithmetic and comparison share this shape: both operands must be numbers.
 * JavaScript would coerce and often produce NaN; an error at the mistake is
 * far easier to debug than a NaN that spreads silently. */
#define BINARY_NUMERIC_OP(valueType, op)                                    \
  do {                                                                      \
    if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {             \
      csVMRuntimeError("operands of '" #op "' must be numbers, got %s and %s", \
                       csValueTypeName(peekStack(1)),                       \
                       csValueTypeName(peekStack(0)));                      \
      return CS_RUNTIME_ERROR;                                              \
    }                                                                       \
    double b = AS_NUMBER(csVMPop());                                        \
    double a = AS_NUMBER(csVMPop());                                        \
    csVMPush(valueType(a op b));                                            \
  } while (false)

#ifdef CS_DEBUG_TRACE_EXECUTION
#define VM_TRACE_STEP()                                        \
  do {                                                         \
    printf("          ");                                      \
    for (Value *slot = vm.stack; slot < vm.stackTop; slot++) { \
      printf("[ ");                                            \
      csValuePrint(*slot);                                     \
      printf(" ]");                                            \
    }                                                          \
    printf("\n");                                              \
    csDisassembleInstruction(&frame->closure->function->chunk,                \
                             (int)(frame->ip - frame->closure->function->chunk.code)); \
  } while (false)
#else
#define VM_TRACE_STEP() ((void)0)
#endif

  uint8_t instruction = 0;

/* Defined outside the dispatch fork so both the computed-goto and the switch
 * path use the same profiling hook. */
#ifdef CS_DEBUG_PROFILE_OPCODES
#define VM_PROFILE_STEP() recordOpcode(instruction)
#else
#define VM_PROFILE_STEP() ((void)0)
#endif

#ifdef CS_COMPUTED_GOTO
  /* Generated from the same list as the OpCode enum, so the table can never
   * drift out of order with it. */
  static const void *const dispatchTable[] = {
#define CS_DISPATCH_ENTRY(name) &&label_##name,
      CS_OPCODE_LIST(CS_DISPATCH_ENTRY)
#undef CS_DISPATCH_ENTRY
  };
  _Static_assert(sizeof(dispatchTable) / sizeof(dispatchTable[0]) == OP_COUNT,
                 "dispatch table and OpCode enum disagree");

#define VM_DISPATCH()                            \
  do {                                           \
    VM_TRACE_STEP();                             \
    instruction = READ_BYTE();                   \
    VM_PROFILE_STEP();                           \
    goto *dispatchTable[instruction];            \
  } while (false)

#define VM_BEGIN VM_DISPATCH();
#define VM_CASE(name) label_##name:
#define VM_NEXT() VM_DISPATCH()
#define VM_END
#else
#define VM_BEGIN         \
  for (;;) {             \
    VM_TRACE_STEP();     \
    instruction = READ_BYTE(); \
    VM_PROFILE_STEP();   \
    switch (instruction) {
#define VM_CASE(name) case name:
#define VM_NEXT() break
#define VM_END \
  }            \
  }
#endif

/* A call may have failed because something inside it threw to a handler this
 * loop owns; if so, take the throw here rather than aborting. */
#define HANDLE_FAILED_CALL()                                            \
  do {                                                                  \
    if (!vm.hasPendingException) return CS_RUNTIME_ERROR;               \
    vm.hasPendingException = false;                                     \
    Value pending = vm.pendingException;                                \
    switch (performThrow(pending, baseFrame, &frame)) {                 \
      case THROW_HANDLED: break;                                        \
      case THROW_PROPAGATE: return CS_RUNTIME_ERROR;                    \
      case THROW_UNCAUGHT:                                              \
        return uncaught(pending);                                       \
    }                                                                   \
  } while (false)

  VM_BEGIN
      VM_CASE(OP_CONSTANT)  csVMPush(READ_CONSTANT()); VM_NEXT();
      VM_CASE(OP_NULL)      csVMPush(NULL_VAL); VM_NEXT();
      VM_CASE(OP_UNDEFINED) csVMPush(UNDEFINED_VAL); VM_NEXT();
      VM_CASE(OP_TRUE)      csVMPush(BOOL_VAL(true)); VM_NEXT();
      VM_CASE(OP_FALSE)     csVMPush(BOOL_VAL(false)); VM_NEXT();

      VM_CASE(OP_POP) csVMPop(); VM_NEXT();
      VM_CASE(OP_POP_N) vm.stackTop -= READ_BYTE(); VM_NEXT();
      VM_CASE(OP_DUP) csVMPush(peekStack(0)); VM_NEXT();

      VM_CASE(OP_DEFINE_GLOBAL) {
        ObjString *name = READ_STRING();
        csTableSet(&frameModule(frame)->globals, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_DEFINE_CONST) {
        ObjString *name = READ_STRING();
        ObjModule *module = frameModule(frame);
        csTableSet(&module->globals, name, peekStack(0));
        csTableSet(&module->globalConsts, name, BOOL_VAL(true));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_GET_GLOBAL) {
        ObjString *name = READ_STRING();
        GlobalCache *cache = READ_GLOBAL_CACHE();

        /* Globals are declared once and never deleted, so after this site has
         * run once the entry it wants is fixed. The version check is what
         * makes holding a pointer into the table safe: only a rehash or a
         * delete can move an entry, and both bump it. */
        if (cache->filled && cache->version == cache->table->version) {
          csVMPush(cache->entry->value);
          VM_NEXT();
        }

        ObjModule *module = frameModule(frame);
        Entry *entry = csTableFindEntry(&module->globals, name);
        if (entry == NULL) {
          /* JavaScript would return undefined for a bare read in sloppy mode.
           * Reading a name that was never declared is a typo, not an intent. */
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        cache->table = &module->globals;
        cache->entry = entry;
        cache->version = module->globals.version;
        cache->filled = true;
        csVMPush(entry->value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_GLOBAL) {
        ObjString *name = READ_STRING();
        GlobalCache *cache = READ_GLOBAL_CACHE();
        if (cache->filled && cache->version == cache->table->version) {
          cache->entry->value = peekStack(0);
          VM_NEXT();
        }
        if (!assignGlobal(frameModule(frame), name, cache, peekStack(0))) {
          return CS_RUNTIME_ERROR;
        }
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL) csVMPush(frame->slots[READ_BYTE()]); VM_NEXT();

      VM_CASE(OP_SET_LOCAL) {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peekStack(0);
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL_LOCAL) {
        Value *slots = frame->slots;
        uint8_t a = READ_BYTE();
        uint8_t b = READ_BYTE();
        csVMPush(slots[a]);
        csVMPush(slots[b]);
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL_CONST) {
        uint8_t slot = READ_BYTE();
        csVMPush(frame->slots[slot]);
        csVMPush(READ_CONSTANT());
        VM_NEXT();
      }

      VM_CASE(OP_SET_LOCAL_POP) {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_SET_GLOBAL_POP) {
        ObjString *name = READ_STRING();
        GlobalCache *cache = READ_GLOBAL_CACHE();
        if (cache->filled && cache->version == cache->table->version) {
          cache->entry->value = csVMPop();
          VM_NEXT();
        }
        if (!assignGlobal(frameModule(frame), name, cache, peekStack(0))) {
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_INC_LOCAL)
      VM_CASE(OP_DEC_LOCAL) {
        uint8_t slot = READ_BYTE();
        Value *target = &frame->slots[slot];
        if (!IS_NUMBER(*target)) {
          csVMRuntimeError("operand of '%s' must be a number, got %s",
                           instruction == OP_INC_LOCAL ? "++" : "--",
                           csValueTypeName(*target));
          return CS_RUNTIME_ERROR;
        }
        /* Goes through the macros rather than mutating the payload in place,
         * so this works under either Value representation. */
        *target = NUMBER_VAL(AS_NUMBER(*target) +
                             (instruction == OP_INC_LOCAL ? 1 : -1));
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL_PROPERTY) {
        /* `this.x` and `local.x`: the receiver is read straight out of its slot
         * rather than pushed and popped. Its own handler rather than a jump
         * into OP_GET_PROPERTY, because the dispatch table's indirect branch is
         * predicted per opcode and sharing one body loses that. */
        Value receiver = frame->slots[READ_BYTE()];
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();

        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          csVMPush(AS_OBJECT(receiver)->as.slots.values[cache->slot]);
          VM_NEXT();
        }

        Value value;
        if (!propertyReadSlow(name, cache, receiver, &value)) return CS_RUNTIME_ERROR;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_GET_PROPERTY) {
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value receiver = peekStack(0);

        /* The point of the whole shape machinery: when this site sees the
         * layout it saw last time — which is what happens when every object
         * reaching it came from the same literal — reading a property is a
         * pointer compare and an indexed load. */
        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          vm.stackTop[-1] = AS_OBJECT(receiver)->as.slots.values[cache->slot];
          VM_NEXT();
        }

        Value value;
        if (!propertyReadSlow(name, cache, receiver, &value)) return CS_RUNTIME_ERROR;
        vm.stackTop[-1] = value;
        VM_NEXT();
      }

      VM_CASE(OP_SET_PROPERTY) {
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value value = peekStack(0);
        Value receiver = peekStack(1);

        /* Overwriting a property that already exists cannot change the shape
         * and cannot allocate. It also cannot be a built-in namespace member:
         * that case errors out below, so such a site never fills its cache. */
        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          AS_OBJECT(receiver)->as.slots.values[cache->slot] = value;
          vm.stackTop -= 2;
          csVMPush(value);
          VM_NEXT();
        }

        if (!propertyWriteSlow(name, cache, receiver, value)) return CS_RUNTIME_ERROR;
        /* Leave the assigned value: assignment is an expression. */
        vm.stackTop -= 2;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_PROPERTY_POP) {
        /* The same store, in statement position: the value nothing reads is
         * never written back. */
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value value = peekStack(0);
        Value receiver = peekStack(1);

        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          AS_OBJECT(receiver)->as.slots.values[cache->slot] = value;
          vm.stackTop -= 2;
          VM_NEXT();
        }

        if (!propertyWriteSlow(name, cache, receiver, value)) return CS_RUNTIME_ERROR;
        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_GET_INDEX) {
        Value index = peekStack(0);
        Value target = peekStack(1);

        if (IS_ARRAY(target)) {
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("array index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjArray *array = AS_ARRAY(target);
          int slot = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          /* Out of range reads as undefined rather than trapping, matching the
           * property rule above. */
          csVMPush(slot >= 0 && slot < array->elements.count
                       ? array->elements.values[slot]
                       : UNDEFINED_VAL);
          VM_NEXT();
        }

        if (IS_STRING(target)) {
          /* Indexing a string yields a one-character string, as in JavaScript,
           * and out of range is undefined rather than an error. */
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("string index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjString *string = AS_STRING(target);
          int slot = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (slot < 0 || slot >= string->length) {
            csVMPush(UNDEFINED_VAL);
          } else {
            csVMPush(OBJ_VAL(csStringCopy(string->chars + slot, 1)));
          }
          VM_NEXT();
        }

        if (IS_OBJECT(target)) {
          if (!IS_STRING(index)) {
            csVMRuntimeError("object key must be a string, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          Value value;
          bool found = csObjectGet(AS_OBJECT(target), AS_STRING(index), &value);
          vm.stackTop -= 2;
          csVMPush(found ? value : UNDEFINED_VAL);
          VM_NEXT();
        }

        csVMRuntimeError("cannot index %s", csValueTypeName(target));
        return CS_RUNTIME_ERROR;
      }

      VM_CASE(OP_ENUM_KEYS) {
        Value target = peekStack(0);
        ObjArray *keys = csArrayNew();
        csVMPush(OBJ_VAL(keys)); /* rooted while the copies below allocate */

        if (IS_OBJECT(target)) {
          ObjObject *object = AS_OBJECT(target);
          for (int i = 0; i < csObjectCount(object); i++) {
            csValueArrayWrite(&keys->elements, OBJ_VAL(csObjectKeyAt(object, i)));
          }
        } else if (IS_ARRAY(target)) {
          /* JavaScript enumerates an array's indices, as strings. */
          for (int i = 0; i < AS_ARRAY(target)->elements.count; i++) {
            char digits[16];
            int length = snprintf(digits, sizeof digits, "%d", i);
            /* The new string is reachable from nothing while the array grows,
             * and growing allocates. */
            ObjString *index = csStringCopy(digits, length);
            csPushTempRoot((Obj *)index);
            csValueArrayWrite(&keys->elements, OBJ_VAL(index));
            csPopTempRoot();
          }
        } else if (!IS_NULL(target) && !IS_UNDEFINED(target)) {
          csVMRuntimeError("'for...in' needs an object or an array, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 2;
        csVMPush(OBJ_VAL(keys));
        VM_NEXT();
      }

      VM_CASE(OP_ITER_LENGTH) {
        Value iterable = csVMPop();
        if (IS_ARRAY(iterable)) {
          csVMPush(NUMBER_VAL(AS_ARRAY(iterable)->elements.count));
          VM_NEXT();
        }
        if (IS_STRING(iterable)) {
          csVMPush(NUMBER_VAL(AS_STRING(iterable)->length));
          VM_NEXT();
        }
        csVMRuntimeError("%s is not iterable", csValueTypeName(iterable));
        return CS_RUNTIME_ERROR;
      }

      VM_CASE(OP_SET_INDEX) {
        Value value = peekStack(0);
        Value index = peekStack(1);
        Value target = peekStack(2);

        if (IS_ARRAY(target)) {
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("array index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjArray *array = AS_ARRAY(target);
          int slot = (int)AS_NUMBER(index);
          if (slot < 0) {
            csVMRuntimeError("array index %d is out of range", slot);
            return CS_RUNTIME_ERROR;
          }
          /* Writing past the end extends the array with undefined rather than
           * creating a hole, so it stays dense. */
          while (array->elements.count <= slot) {
            csValueArrayWrite(&array->elements, UNDEFINED_VAL);
          }
          array->elements.values[slot] = value;
        } else if (IS_OBJECT(target)) {
          if (!IS_STRING(index)) {
            csVMRuntimeError("object key must be a string, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          /* `console["log"] = f` is the same act as `console.log = f`. */
          if (AS_OBJECT(target)->frozen) {
            csVMRuntimeError("'%s.%s' is a built-in and cannot be replaced",
                             AS_OBJECT(target)->name->chars, AS_STRING(index)->chars);
            return CS_RUNTIME_ERROR;
          }
          csObjectPut(AS_OBJECT(target), AS_STRING(index), value);
        } else {
          csVMRuntimeError("cannot index %s", csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 3;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT) {
        int count = READ_BYTE();
        ObjObject *object = csObjectNew("Object");
        /* The literal's keys and values are still on the stack, so they are
         * rooted; the new object is not until it is pushed. */
        csPushTempRoot((Obj *)object);

        Value *entries = vm.stackTop - (count * 2);
        for (int i = 0; i < count; i++) {
          Value key = entries[i * 2];
          if (!IS_STRING(key)) {
            /* A computed key can be anything; JavaScript converts it. */
            size_t length = 0;
            char *text = csValueToCString(key, &length);
            if (text == NULL) {
              csVMRuntimeError("out of memory building an object key");
              return CS_RUNTIME_ERROR;
            }
            ObjString *converted = csStringCopy(text, (int)length);
            free(text);
            entries[i * 2] = OBJ_VAL(converted); /* keeps it rooted */
            key = entries[i * 2];
          }
          csObjectPut(object, AS_STRING(key), entries[i * 2 + 1]);
        }

        csPopTempRoot();
        vm.stackTop = entries;
        csVMPush(OBJ_VAL(object));
        VM_NEXT();
      }

      VM_CASE(OP_SPREAD_MARK) {
        Value value = peekStack(0);
        if (!IS_ARRAY(value) && !IS_STRING(value)) {
          csVMRuntimeError("only arrays and strings can be spread, got %s",
                           csValueTypeName(value));
          return CS_RUNTIME_ERROR;
        }

        /* Wrap in a marked array so the builder below can tell a spread
         * element from a plain array element. A string spreads to characters. */
        ObjArray *marker = csArrayNew();
        marker->isSpreadMarker = true;
        csPushTempRoot((Obj *)marker);

        if (IS_ARRAY(value)) {
          ObjArray *source = AS_ARRAY(value);
          for (int i = 0; i < source->elements.count; i++) {
            csValueArrayWrite(&marker->elements, source->elements.values[i]);
          }
        } else {
          ObjString *source = AS_STRING(value);
          for (int i = 0; i < source->length; i++) {
            ObjString *piece = csStringCopy(source->chars + i, 1);
            csPushTempRoot((Obj *)piece);
            csValueArrayWrite(&marker->elements, OBJ_VAL(piece));
            csPopTempRoot();
          }
        }

        csPopTempRoot();
        csVMPop();
        csVMPush(OBJ_VAL(marker));
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY_SPREAD) {
        int count = READ_BYTE();
        ObjArray *array = csArrayNew();
        csPushTempRoot((Obj *)array);

        Value *elements = vm.stackTop - count;
        for (int i = 0; i < count; i++) {
          if (IS_ARRAY(elements[i]) && AS_ARRAY(elements[i])->isSpreadMarker) {
            ObjArray *spread = AS_ARRAY(elements[i]);
            for (int j = 0; j < spread->elements.count; j++) {
              csValueArrayWrite(&array->elements, spread->elements.values[j]);
            }
          } else {
            csValueArrayWrite(&array->elements, elements[i]);
          }
        }

        csPopTempRoot();
        vm.stackTop = elements;
        csVMPush(OBJ_VAL(array));
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY_REST) {
        uint8_t from = READ_BYTE();
        /* Replaces the source on top with the slice, the same shape
         * OP_ITER_LENGTH uses, so no stack fixup is needed afterwards. */
        Value source = csVMPop();
        if (!IS_ARRAY(source)) {
          csVMRuntimeError("cannot destructure %s as an array",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        ObjArray *rest = csArrayNew();
        csPushTempRoot((Obj *)rest);
        ObjArray *array = AS_ARRAY(source);
        for (int i = from; i < array->elements.count; i++) {
          csValueArrayWrite(&rest->elements, array->elements.values[i]);
        }
        csPopTempRoot();

        csVMPush(OBJ_VAL(rest));
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY) {
        int count = READ_BYTE();
        ObjArray *array = csArrayNew();
        csPushTempRoot((Obj *)array);

        Value *elements = vm.stackTop - count;
        for (int i = 0; i < count; i++) {
          csValueArrayWrite(&array->elements, elements[i]);
        }

        csPopTempRoot();
        vm.stackTop = elements;
        csVMPush(OBJ_VAL(array));
        VM_NEXT();
      }

      VM_CASE(OP_CALL) {
        int argCount = READ_BYTE();
        if (!callValue(peekStack(argCount), argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* A user call pushed a frame, so the cached pointer is stale. */
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_CALL_SPREAD) {
        Value packed = csVMPop();
        if (!IS_ARRAY(packed)) {
          csVMRuntimeError("spread arguments must be an array");
          return CS_RUNTIME_ERROR;
        }

        ObjArray *args = AS_ARRAY(packed);
        int argCount = args->elements.count;
        if (argCount > UINT8_MAX) {
          csVMRuntimeError("too many arguments after spreading (limit %d)", UINT8_MAX);
          return CS_RUNTIME_ERROR;
        }

        /* The array is unreachable once popped, so root it while its elements
         * are copied onto the stack. */
        csPushTempRoot((Obj *)args);
        for (int i = 0; i < argCount; i++) csVMPush(args->elements.values[i]);
        csPopTempRoot();

        if (!callValue(peekStack(argCount), argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_INVOKE) {
        ObjString *name = READ_STRING();
        int argCount = READ_BYTE();
        if (!invokeMethod(peekStack(argCount), name, argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* A method that turned out to be user code pushed a frame. */
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_CLASS) {
        csVMPush(OBJ_VAL(csClassNew(READ_STRING())));
        VM_NEXT();
      }

      VM_CASE(OP_INHERIT) {
        Value superclass = peekStack(1);
        if (!IS_CLASS(superclass)) {
          csVMRuntimeError("a class can only extend another class, got %s",
                           csValueTypeName(superclass));
          return CS_RUNTIME_ERROR;
        }
        AS_CLASS(peekStack(0))->superclass = AS_CLASS(superclass);
        /* The superclass stays behind as the hidden local that `super` reads. */
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_METHOD)
      VM_CASE(OP_STATIC_METHOD) {
        ObjString *name = READ_STRING();
        ObjClass *klass = AS_CLASS(peekStack(1));
        /* Both stay on the stack across the insert, which can allocate. */
        csTableSet(instruction == OP_METHOD ? &klass->methods : &klass->statics,
                   name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_GETTER)
      VM_CASE(OP_SETTER) {
        ObjString *name = READ_STRING();
        ObjClass *klass = AS_CLASS(peekStack(1));
        csTableSet(instruction == OP_GETTER ? &klass->getters : &klass->setters, name,
                   peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_STATIC_FIELD) {
        ObjString *name = READ_STRING();
        csTableSet(&AS_CLASS(peekStack(1))->statics, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_CONSTRUCTOR) {
        AS_CLASS(peekStack(1))->initializer = AS_CLOSURE(peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_FIELD_INIT) {
        AS_CLASS(peekStack(1))->fieldInit = AS_CLOSURE(peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_NEW) {
        int argCount = READ_BYTE();
        Value target = peekStack(argCount);

        /* A built-in constructor is an ordinary native that returns the thing
         * it made, so `new Promise(executor)` and `Promise(executor)` are the
         * same call. There is no separate construction protocol to write. */
        if (IS_NATIVE(target)) {
          if (!callNative(AS_NATIVE(target), UNDEFINED_VAL, argCount)) {
            HANDLE_FAILED_CALL();
          }
          VM_NEXT();
        }

        if (!IS_CLASS(target)) {
          csVMRuntimeError("'new' needs a class, got %s", csValueTypeName(target));
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }

        ObjClass *klass = AS_CLASS(target);
        ObjObject *instance = csInstanceNew(klass);
        /* The class slot becomes the instance, which puts it exactly where a
         * constructor's slot 0 — its `this` — will be. */
        vm.stackTop[-argCount - 1] = OBJ_VAL(instance);

        ObjClass *owner = findConstructorOwner(klass);

        if (owner == NULL) {
          /* Nothing in the hierarchy declares a constructor, so all there is
           * to do is initialise the fields. */
          if (argCount != 0) {
            csVMRuntimeError("%s has no constructor but was given %d argument%s",
                             klass->name->chars, argCount, argCount == 1 ? "" : "s");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          if (!runFieldInitializers(klass, NULL, OBJ_VAL(instance))) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          /* The instance is already sitting where the result belongs. */
          VM_NEXT();
        }

        /* Classes between the instance's own and the one that owns the
         * constructor have implicit constructors, whose only job is to run
         * their fields — after the inherited constructor returns. */
        bool pendingFields = false;
        for (ObjClass *c = klass; c != owner; c = c->superclass) {
          if (c->fieldInit != NULL) {
            pendingFields = true;
            break;
          }
        }

        if (!pendingFields) {
          /* The common shape. A constructor returns `this`, so the frame it
           * leaves behind is the instance — no opcode needed to recover it. */
          if (!callMethod(owner->initializer, argCount)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          VM_NEXT();
        }

        /* Otherwise the constructor has to finish before those fields run, so
         * it runs in a loop of its own and the fields follow it. */
        csPushTempRoot((Obj *)instance);
        Value ignored;
        bool ok = csVMCallCallback(OBJ_VAL(owner->initializer), argCount, &ignored) &&
                  runFieldInitializers(klass, owner, OBJ_VAL(instance));
        csPopTempRoot();
        if (!ok) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        csVMPush(OBJ_VAL(instance));
        VM_NEXT();
      }

      VM_CASE(OP_GET_SUPER) {
        ObjString *name = READ_STRING();
        ObjClass *superclass = AS_CLASS(peekStack(0));

        /* `super.x` where the superclass defines a getter runs it, rather than
         * handing back something to call. */
        ObjClosure *getter = IS_CLASS(peekStack(1))
                                 ? NULL
                                 : csClassFindGetter(superclass, name);
        if (getter != NULL) {
          Value receiver = peekStack(1);
          vm.stackTop -= 2;
          csVMPush(receiver);
          Value value;
          if (!csVMCallCallback(OBJ_VAL(getter), 0, &value)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          csVMPush(value);
          VM_NEXT();
        }

        ObjClosure *method =
            findSuperMember(superclass, name, IS_CLASS(peekStack(1)));
        if (method == NULL) {
          csVMRuntimeError("class %s has no member '%s'", superclass->name->chars,
                           name->chars);
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        ObjBoundMethod *bound = csBoundMethodNew(peekStack(1), (Obj *)method);
        vm.stackTop -= 2;
        csVMPush(OBJ_VAL(bound));
        VM_NEXT();
      }

      VM_CASE(OP_SUPER_INVOKE) {
        ObjString *name = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(csVMPop());
        ObjClosure *method =
            findSuperMember(superclass, name, IS_CLASS(peekStack(argCount)));
        if (method == NULL) {
          csVMRuntimeError("class %s has no member '%s'", superclass->name->chars,
                           name->chars);
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* `this` is still sitting below the arguments, which is where the
         * method's slot 0 has to be. */
        if (!callMethod(method, argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_SUPER_CALL) {
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(csVMPop());
        ObjClosure *constructor = findConstructor(superclass);

        if (constructor == NULL) {
          if (argCount != 0) {
            csVMRuntimeError("%s has no constructor but super() was given "
                             "%d argument%s",
                             superclass->name->chars, argCount,
                             argCount == 1 ? "" : "s");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          /* Drop `this` along with the arguments and stand in for the value
           * the constructor would have returned. */
          vm.stackTop -= argCount + 1;
          csVMPush(UNDEFINED_VAL);
          VM_NEXT();
        }

        if (!callMethod(constructor, argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_INSTANCEOF) {
        Value target = peekStack(0);
        Value value = peekStack(1);
        if (!IS_CLASS(target)) {
          csVMRuntimeError("the right side of 'instanceof' must be a class, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        bool result = IS_OBJECT(value) && AS_OBJECT(value)->klass != NULL &&
                      csClassDescendsFrom(AS_OBJECT(value)->klass, AS_CLASS(target));
        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(result));
        VM_NEXT();
      }

      VM_CASE(OP_IMPORT_NAME) {
        ObjString *name = READ_STRING();
        ObjModule *module = AS_MODULE(peekStack(0));
        Value value;
        if (!csTableGet(&module->globals, name, &value)) {
          /* The compiler checked the export list, so reaching here means the
           * module did not get as far as defining it. */
          csVMRuntimeError("'%s' was not defined by '%s'", name->chars,
                           module->path->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_IMPORT_NAMESPACE) {
        ObjModule *module = AS_MODULE(peekStack(0));

        /* Sorted, because that is what the specification says a namespace's
         * keys are — the exporting file's declaration order is deliberately
         * not observable through one. Collected first so the sort does not
         * have to happen against a hash table. */
        ObjString *names[CS_NAMESPACE_MAX];
        int count = 0;
        for (int i = 0; i < module->exports.capacity; i++) {
          Entry *entry = &module->exports.entries[i];
          if (entry->key == NULL) continue;
          if (count == CS_NAMESPACE_MAX) {
            csVMRuntimeError("'%s' exports more than %d names",
                             module->path->chars, CS_NAMESPACE_MAX);
            return CS_RUNTIME_ERROR;
          }
          names[count++] = entry->key;
        }

        /* Insertion sort: export lists are short, and this keeps the
         * comparison — code-unit order, shorter first on a prefix — in one
         * readable place. */
        for (int i = 1; i < count; i++) {
          ObjString *key = names[i];
          int j = i - 1;
          while (j >= 0) {
            int shorter = names[j]->length < key->length ? names[j]->length : key->length;
            int order = memcmp(names[j]->chars, key->chars, (size_t)shorter);
            if (order == 0) order = names[j]->length - key->length;
            if (order <= 0) break;
            names[j + 1] = names[j];
            j--;
          }
          names[j + 1] = key;
        }

        ObjObject *namespaceObject = csObjectNew(module->path->chars);
        /* Kept on the stack under the module so the copies below, which
         * allocate, cannot collect it. */
        csVMPush(OBJ_VAL(namespaceObject));

        for (int i = 0; i < count; i++) {
          Value value;
          if (!csTableGet(&module->globals, names[i], &value)) continue;
          csObjectPut(namespaceObject, names[i], value);
        }

        /* A namespace is a view of another module, not somewhere to put
         * things. */
        csObjectFreeze(namespaceObject);
        vm.stackTop -= 2;
        csVMPush(OBJ_VAL(namespaceObject));
        VM_NEXT();
      }

      VM_CASE(OP_AWAIT) {
        ObjFiber *fiber = vm.currentFiber;
        if (fiber == NULL) {
          csVMRuntimeError("'await' outside an async function");
          return CS_RUNTIME_ERROR;
        }

        Value awaited = peekStack(0);
        ObjPromise *promise;
        if (IS_PROMISE(awaited)) {
          promise = AS_PROMISE(awaited);
        } else {
          /* Awaiting a plain value still yields to the microtask queue, which
           * is what keeps `await 1` from running its continuation early. */
          promise = csPromiseNew();
        }

        /* Rooted before anything else: settling and registering both allocate,
         * and once the value comes off the stack this is the only reference. */
        csPushTempRoot((Obj *)promise);
        if (!IS_PROMISE(awaited)) csPromiseFulfill(promise, awaited);
        csVMPop();

        promise->handled = true;
        csPromiseAddReaction(promise, UNDEFINED_VAL, UNDEFINED_VAL, NULL);
        if (promise->state == PROMISE_PENDING) {
          promise->reactions[promise->reactionCount - 1].fiber = fiber;
        } else {
          vm.microtasks[vm.microtaskCount - 1].fiber = fiber;
        }
        csPopTempRoot();

        /* Hand control back to whoever started this fiber. The frame's ip is
         * already past this instruction, so resuming lands on the next one and
         * the awaited value arrives in the promise's place. */
        vm.fiberSuspended = true;
        return CS_OK;
      }

      VM_CASE(OP_CLOSURE) {
        ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure *closure = csClosureNew(function);
        csVMPush(OBJ_VAL(closure));

        /* The compiler emitted one (isLocal, index) pair per upvalue. A local
         * is captured from this frame; anything else is already an upvalue of
         * the enclosing closure and is simply shared. */
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          closure->upvalues[i] = isLocal ? captureUpvalue(frame->slots + index)
                                         : frame->closure->upvalues[index];
        }
        VM_NEXT();
      }

      VM_CASE(OP_GET_UPVALUE) {
        uint8_t slot = READ_BYTE();
        csVMPush(*frame->closure->upvalues[slot]->location);
        VM_NEXT();
      }

      VM_CASE(OP_SET_UPVALUE) {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peekStack(0);
        VM_NEXT();
      }

      VM_CASE(OP_CLOSE_UPVALUE)
        closeUpvalues(vm.stackTop - 1);
        csVMPop();
        VM_NEXT();

      VM_CASE(OP_ADD_NUM) {
        /* The checker guaranteed both operands are numbers, so there is
         * nothing to test. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        vm.stackTop -= 2;
        csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
        VM_NEXT();
      }

      VM_CASE(OP_ADD) {
        /* Adding two numbers is overwhelmingly the common case, and routing it
         * through concatenateOrAdd() costs a call plus two string checks. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        if (IS_NUMBER(a) && IS_NUMBER(b)) {
          vm.stackTop -= 2;
          csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
          VM_NEXT();
        }
        if (!concatenateOrAdd()) return CS_RUNTIME_ERROR;
        VM_NEXT();
      }

      VM_CASE(OP_SUBTRACT) BINARY_NUMERIC_OP(NUMBER_VAL, -); VM_NEXT();
      VM_CASE(OP_MULTIPLY) BINARY_NUMERIC_OP(NUMBER_VAL, *); VM_NEXT();
      VM_CASE(OP_DIVIDE)   BINARY_NUMERIC_OP(NUMBER_VAL, /); VM_NEXT();

      VM_CASE(OP_MODULO) {
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '%%' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double b = AS_NUMBER(csVMPop());
        double a = AS_NUMBER(csVMPop());

        /* fmod() is a libm call and costs roughly 20ns; integer remainder is
         * about 1ns. Loop counters are integers virtually always, so take the
         * integer path whenever it is exactly equivalent.
         *
         * C's % truncates toward zero and so keeps the sign of the dividend,
         * which is what JavaScript's % does too. A zero dividend is excluded so
         * that -0 %% n stays -0 rather than becoming +0. */
        if (b != 0 && a != 0 && isExactInteger(a) && isExactInteger(b)) {
          csVMPush(NUMBER_VAL((double)((int64_t)a % (int64_t)b)));
        } else {
          csVMPush(NUMBER_VAL(fmod(a, b)));
        }
        VM_NEXT();
      }

      VM_CASE(OP_EXPONENT) {
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '**' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double exponent = AS_NUMBER(csVMPop());
        double base = AS_NUMBER(csVMPop());
        csVMPush(NUMBER_VAL(pow(base, exponent)));
        VM_NEXT();
      }

      VM_CASE(OP_NEGATE)
        if (!IS_NUMBER(peekStack(0))) {
          csVMRuntimeError("operand of unary '-' must be a number, got %s",
                           csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(NUMBER_VAL(-AS_NUMBER(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_NOT)
        csVMPush(BOOL_VAL(!csValueIsTruthy(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_TYPEOF) {
        const char *name = csValueTypeName(csVMPop());
        csVMPush(OBJ_VAL(csStringCopy(name, (int)strlen(name))));
        VM_NEXT();
      }

      VM_CASE(OP_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(csValuesStrictEqual(a, b)));
        VM_NEXT();
      }
      VM_CASE(OP_NOT_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(!csValuesStrictEqual(a, b)));
        VM_NEXT();
      }

      VM_CASE(OP_GREATER)       BINARY_NUMERIC_OP(BOOL_VAL, >); VM_NEXT();
      VM_CASE(OP_GREATER_EQUAL) BINARY_NUMERIC_OP(BOOL_VAL, >=); VM_NEXT();
      VM_CASE(OP_LESS)          BINARY_NUMERIC_OP(BOOL_VAL, <); VM_NEXT();
      VM_CASE(OP_LESS_EQUAL)    BINARY_NUMERIC_OP(BOOL_VAL, <=); VM_NEXT();

      VM_CASE(OP_JUMP) {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_TRUE) {
        uint16_t offset = READ_SHORT();
        if (csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_POP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(csVMPop())) frame->ip += offset;
        VM_NEXT();
      }
/* Fused compare-and-branch. The operand check is the same one the standalone
 * comparison performs, so the error message a program sees does not change. */
#define COMPARE_JUMP(op)                                                       \
  do {                                                                         \
    uint16_t offset = READ_SHORT();                                            \
    Value b = peekStack(0);                                                    \
    Value a = peekStack(1);                                                    \
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) {                                      \
      csVMRuntimeError("operands of '" #op "' must be numbers, got %s and %s", \
                       csValueTypeName(a), csValueTypeName(b));                 \
      return CS_RUNTIME_ERROR;                                                 \
    }                                                                          \
    vm.stackTop -= 2;                                                          \
    if (!(AS_NUMBER(a) op AS_NUMBER(b))) frame->ip += offset;                   \
  } while (false)

/* VM_NEXT stays outside the macro. Under computed goto it is a goto, but under
 * switch dispatch it is a `break` — which a do/while(false) would swallow,
 * falling through to the next case instead of dispatching. */
      VM_CASE(OP_JUMP_IF_NOT_LESS) COMPARE_JUMP(<); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_LESS_EQUAL) COMPARE_JUMP(<=); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER) COMPARE_JUMP(>); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER_EQUAL) COMPARE_JUMP(>=); VM_NEXT();

      /* Equality accepts any types, so it needs no operand check. */
      VM_CASE(OP_JUMP_IF_NOT_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (!csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

      VM_CASE(OP_JUMP_IF_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

#undef COMPARE_JUMP

      VM_CASE(OP_LOOP) {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        /* A back-edge counts as much as a call: a function called once around
         * a millionfold loop is as hot as one called a million times, and only
         * this catches the first kind. */
        CS_JIT_TICK(frame->closure->function);
        VM_NEXT();
      }

      VM_CASE(OP_TRY) {
        uint16_t offset = READ_SHORT();
        if (vm.handlerCount == vm.handlerCapacity) {
          csVMRuntimeError("too many nested 'try' blocks (limit %d)",
                           vm.handlerCapacity);
          return CS_RUNTIME_ERROR;
        }
        ExceptionHandler *handler = &vm.handlers[vm.handlerCount++];
        handler->frameCount = vm.frameCount;
        handler->stackTop = vm.stackTop;
        handler->ip = frame->ip + offset;
        handler->handlerCount = vm.handlerCount - 1;
        VM_NEXT();
      }

      VM_CASE(OP_END_TRY)
        vm.handlerCount--;
        VM_NEXT();

      VM_CASE(OP_THROW) {
        Value thrown = csVMPop();
        switch (performThrow(thrown, baseFrame, &frame)) {
          case THROW_HANDLED:
            VM_NEXT();
          case THROW_PROPAGATE:
            return CS_RUNTIME_ERROR;
          case THROW_UNCAUGHT:
            return uncaught(thrown);
        }
        VM_NEXT();
      }

      VM_CASE(OP_RETURN) {
        Value result = csVMPop();

        /* Returning past a `try` abandons its handler; leaving it installed
         * would send a later throw back into a frame that no longer exists. */
        while (vm.handlerCount > 0 &&
               vm.handlers[vm.handlerCount - 1].frameCount >= vm.frameCount) {
          vm.handlerCount--;
        }

        /* Anything this frame's locals were captured into has to move to the
         * heap before the slots are reused. */
        closeUpvalues(frame->slots);
        vm.frameCount--;

        /* Hand control back to whoever started this loop: the driver at depth
         * 0, or a native that called into user code at some deeper level. The
         * result is left on the stack in place of the callee. */
        if (vm.frameCount == baseFrame) {
          vm.stackTop = frame->slots;
          csVMPush(result);
          return CS_OK;
        }

        /* Discard the whole window, then leave the result where the callee was. */
        vm.stackTop = frame->slots;
        csVMPush(result);
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

#ifndef CS_COMPUTED_GOTO
      default:
        /* Unreachable: the only producer of bytecode is our own compiler. The
         * computed-goto path has no equivalent guard — an out-of-range byte
         * would index past the table — which is the price of the faster
         * dispatch, and is acceptable because chunks never come from disk. */
        csVMRuntimeError("unknown opcode %d", instruction);
        return CS_RUNTIME_ERROR;
#endif

  VM_END

#undef VM_END
#undef VM_NEXT
#undef VM_CASE
#undef VM_BEGIN
#undef HANDLE_FAILED_CALL
#undef VM_PROFILE_STEP
#undef VM_TRACE_STEP
#undef BINARY_NUMERIC_OP
#undef READ_STRING
#undef READ_CONSTANT
#undef READ_SHORT
#undef READ_BYTE
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

InterpretResult csInterpret(const char *source, const char *sourceName) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, sourceName);
  vm.sourceName = sourceName;

#ifdef CS_DEBUG_PRINT_TOKENS
  csLexerDumpTokens(source, &diag);
  csDiagnosticsInit(&diag, source, sourceName); /* reset after the dry run */
#endif

  AstArena arena;
  csAstArenaInit(&arena);

  AstNode *program = csParse(source, &arena, &diag);
  if (program == NULL) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

  /* Code with no file of its own still resolves its imports, against the
   * working directory. */
  if (!csModuleLoadImports(program, vm.mainModule->path->chars, &diag)) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

  /* Static checking sits between parsing and code generation: it needs the
   * whole tree, and the compiler benefits from the types it resolves. */
  if (!csTypeCheck(program, &diag)) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

#ifdef CS_DEBUG_PRINT_AST
  csAstPrint(program);
#endif

  ObjFunction *script = csCompile(program, vm.mainModule, &diag);
  /* The AST is only needed to produce bytecode, so it goes as soon as it has. */
  csAstArenaFree(&arena);
  if (script == NULL) return CS_COMPILE_ERROR;

#ifdef CS_DEBUG_PRINT_CODE
  csDisassembleChunk(&script->chunk, sourceName);
#endif

  InterpretResult pending = csVMRunPendingModules();
  if (pending != CS_OK) return pending;

  InterpretResult result = csVMRunBody(script);
  if (result != CS_OK) return result;
  return csVMRunEventLoop();
}

InterpretResult csVMRunBody(ObjFunction *body) {
  /* A top level is itself a function, so running it is just a call. Pushing
   * the closure first keeps it reachable while callClosure allocates nothing
   * but still leaves it rooted through the frame. */
  csPushTempRoot((Obj *)body);
  ObjClosure *closure = csClosureNew(body);
  csPopTempRoot();

  csVMPush(OBJ_VAL(closure));
  callClosure(closure, 0);

  InterpretResult result = run(0);
  resetStack();
  return result;
}










InterpretResult csVMRunPendingModules(void) {
  /* Dependency order, so everything a module imports has already run by the
   * time it starts. The list is cleared as it goes rather than at the end, so
   * a module that throws does not leave the rest queued behind it. */
  for (int i = 0; i < vm.pendingCount; i++) {
    ObjModule *module = vm.pending[i];
    if (module->executed) continue;

    ObjFunction *body = module->body;
    module->executed = true;
    module->body = NULL;

    InterpretResult result = csVMRunBody(body);
    if (result != CS_OK) {
      vm.pendingCount = 0;
      return result;
    }
  }
  vm.pendingCount = 0;
  return CS_OK;
}





bool csVMCallAdapted(Value callee, Value *args, int available, Value *result) {
  int wanted = available;
  if (IS_CLOSURE(callee)) {
    wanted = AS_CLOSURE(callee)->function->arity;
  } else if (IS_NATIVE(callee) && AS_NATIVE(callee)->arity >= 0) {
    wanted = AS_NATIVE(callee)->arity;
  }

  csVMPush(callee);
  for (int i = 0; i < wanted; i++) {
    csVMPush(i < available ? args[i] : UNDEFINED_VAL);
  }
  return csVMCallCallback(callee, wanted, result);
}

bool csVMCallCallback(Value callee, int argCount, Value *result) {
  /* A native calls into user code with the callee and its arguments already
   * pushed, exactly as OP_CALL would leave them. */
  int baseFrame = vm.frameCount;

  if (IS_NATIVE(callee)) {
    /* No frame to run — the native answers directly. */
    if (!callNative(AS_NATIVE(callee), UNDEFINED_VAL, argCount)) return false;
    *result = csVMPop();
    return true;
  }

  if (IS_BOUND_METHOD(callee)) {
    /* Unwrap once and re-enter, so a bound method reached through a callback
     * takes exactly the same path as one called directly. */
    ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
    if (bound->method->type == OBJ_NATIVE) {
      if (!callNative((ObjNative *)bound->method, bound->receiver, argCount)) return false;
      *result = csVMPop();
      return true;
    }
    vm.stackTop[-argCount - 1] = bound->receiver;
    return csVMCallCallback(OBJ_VAL(bound->method), argCount, result);
  }

  if (!IS_CLOSURE(callee)) {
    csVMRuntimeError("%s is not a function", csValueTypeName(callee));
    return false;
  }

  /* An async function returns its promise without pushing a frame, so there is
   * no nested loop to run — it either finished or is suspended already. */
  if (AS_CLOSURE(callee)->function->isAsync) {
    if (!callAsyncFunction(AS_CLOSURE(callee), argCount)) return false;
    *result = csVMPop();
    return true;
  }

  if (!callClosure(AS_CLOSURE(callee), argCount)) return false;

  /* Runs until that one call returns, leaving its result on the stack. */
  if (run(baseFrame) != CS_OK) return false;

  *result = csVMPop();
  return true;
}
