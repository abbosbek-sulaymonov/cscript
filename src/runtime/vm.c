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
#include "runtime/vm_internal.h"

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
  vm.fiberYielded = false;
  vm.deferUncaught = false;
  vm.pendingCount = 0;
  csTableInit(&vm.strings);
  csTableInit(&vm.arrayMethods);
  csTableInit(&vm.stringMethods);
  csTableInit(&vm.promiseMethods);
  csTableInit(&vm.mapMethods);
  csTableInit(&vm.generatorMethods);
  csTableInit(&vm.numberMethods);
  csTableInit(&vm.functionMethods);
  csTableInit(&vm.dateMethods);
  csTableInit(&vm.weakMethods);
  csTableInit(&vm.symbolMethods);
  csTableInit(&vm.bigintMethods);
  vm.accessorMarker = NULL;
  vm.pendingNewTarget = UNDEFINED_VAL;
  csTableInit(&vm.symbolRegistry);
  csTableInit(&vm.symbolsByKey);
  vm.iteratorSymbol = NULL;
  vm.asyncIteratorSymbol = NULL;
  csTableInit(&vm.regexMethods);

  vm.microtasks = NULL;
  vm.microtaskCount = 0;
  vm.microtaskCapacity = 0;
  vm.microtaskHead = 0;
  vm.timerCount = 0;
  vm.nextTimerId = 1;
  vm.firingTimerId = -1;
  vm.firingCancelled = false;
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
  vm.accessorMarker = csObjectNew("<accessor>");

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
  csTableFree(&vm.mapMethods);
  csTableFree(&vm.generatorMethods);
  csTableFree(&vm.numberMethods);
  csTableFree(&vm.functionMethods);
  csTableFree(&vm.dateMethods);
  csTableFree(&vm.weakMethods);
  csTableFree(&vm.symbolMethods);
  csTableFree(&vm.bigintMethods);
  csTableFree(&vm.symbolRegistry);
  csTableFree(&vm.symbolsByKey);
  csTableFree(&vm.regexMethods);
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
/* --- BigInt arithmetic -----------------------------------------------------
 *
 * JavaScript keeps BigInt and Number strictly apart under the arithmetic
 * operators: `1n + 1` is a TypeError. That looks unhelpful until you see what
 * the alternative costs — widening the BigInt to a double would silently throw
 * away exactly the precision it was created to keep, and narrowing the number
 * would have to invent an answer for `1n + 0.5`. Refusing is the only choice
 * that cannot be wrong. Ordering is different, and mixes freely: `1n < 2` has
 * one right answer, and comparingExactly() finds it without rounding.
 */

/* Replaces the top two stack slots with the result of `op` applied to them.
 * `symbol` is the operator as written, for the error. */
static bool bigintArithmetic(char op, const char *symbol) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (!IS_BIGINT(a) || !IS_BIGINT(b)) {
    csVMRuntimeError("cannot mix BigInt and %s in '%s'",
                     csValueTypeName(IS_BIGINT(a) ? b : a), symbol);
    return false;
  }

  const BigInt *left = &AS_BIGINT(a)->value;
  const BigInt *right = &AS_BIGINT(b)->value;

  BigInt result;
  csBigInit(&result);
  bool computed = false;
  switch (op) {
    case '+': computed = csBigAdd(&result, left, right); break;
    case '-': computed = csBigSubtract(&result, left, right); break;
    case '*': computed = csBigMultiply(&result, left, right); break;
    case '/':
    case '%':
      if (csBigIsZero(right)) {
        csBigFree(&result);
        csVMRuntimeError("division of a BigInt by zero");
        return false;
      }
      computed = op == '/' ? csBigDivide(&result, left, right)
                           : csBigRemainder(&result, left, right);
      break;
    case 'p':
      if (right->negative) {
        csBigFree(&result);
        csVMRuntimeError("a BigInt cannot be raised to a negative power");
        return false;
      }
      computed = csBigPower(&result, left, right);
      break;
    default: break;
  }

  if (!computed) {
    csBigFree(&result);
    csVMRuntimeError("out of memory in BigInt arithmetic");
    return false;
  }

  /* Allocated while both operands are still on the stack, so a collection
   * triggered here cannot free the limbs being read. */
  ObjBigInt *object = csBigIntNew(result);
  vm.stackTop -= 2;
  csVMPush(OBJ_VAL(object));
  return true;
}

typedef enum {
  ORDER_KNOWN,     /* `*order` holds it */
  ORDER_UNORDERED, /* NaN was involved: every comparison is false */
  ORDER_INVALID,   /* not comparable at all; an error has been reported */
} OrderResult;

/* Orders two values when at least one is a BigInt, without rounding either. */
static OrderResult comparingExactly(Value a, Value b, int *order) {
  if (IS_BIGINT(a) && IS_BIGINT(b)) {
    *order = csBigCompare(&AS_BIGINT(a)->value, &AS_BIGINT(b)->value);
    return ORDER_KNOWN;
  }
  if (IS_BIGINT(a) && IS_NUMBER(b)) {
    *order = csBigCompareDouble(&AS_BIGINT(a)->value, AS_NUMBER(b));
    return *order == 2 ? ORDER_UNORDERED : ORDER_KNOWN;
  }
  if (IS_NUMBER(a) && IS_BIGINT(b)) {
    int reversed = csBigCompareDouble(&AS_BIGINT(b)->value, AS_NUMBER(a));
    if (reversed == 2) return ORDER_UNORDERED;
    *order = -reversed;
    return ORDER_KNOWN;
  }

  csVMRuntimeError("cannot compare %s with %s", csValueTypeName(a),
                   csValueTypeName(b));
  return ORDER_INVALID;
}

bool csVMIsAccessorSlot(Value value) {
  return IS_OBJ(value) && AS_OBJ(value) == (Obj *)vm.accessorMarker;
}

unsigned csVMAccessorKind(ObjObject *object, ObjString *key) {
  if (object->klass == NULL) return 0;
  Value stored;
  if (!csObjectGet(object, key, &stored) || !csVMIsAccessorSlot(stored)) return 0;

  unsigned kind = 0;
  if (csClassFindGetter(object->klass, key) != NULL) kind |= CS_ACCESSOR_GET;
  if (csClassFindSetter(object->klass, key) != NULL) kind |= CS_ACCESSOR_SET;
  return kind;
}

bool csVMReadOwnProperty(ObjObject *object, ObjString *key, Value *out) {
  Value stored;
  if (!csObjectGet(object, key, &stored)) {
    *out = UNDEFINED_VAL;
    return true;
  }
  if (!csVMIsAccessorSlot(stored)) {
    *out = stored;
    return true;
  }

  /* An accessor with no getter reads as undefined, which is what a
   * write-only property is worth. */
  ObjClosure *getter = object->klass != NULL
                           ? csClassFindGetter(object->klass, key)
                           : NULL;
  if (getter == NULL) {
    *out = UNDEFINED_VAL;
    return true;
  }
  csVMPush(OBJ_VAL(object));
  return csVMCallCallback(OBJ_VAL(getter), 0, out);
}

/* An object's own `toString`, if it has one anywhere in its chain.
 *
 * JavaScript calls it whenever an object is converted to a string, and CScript
 * ignoring it was a silent wrong answer rather than a deliberate divergence:
 * the documented difference is only about what an object with *no* toString
 * renders as. Inspection is a separate question and stays as it was — Node's
 * console.log does not call toString either. */
static bool findUserToString(Value value, Value *method) {
  if (!IS_OBJECT(value)) return false;

  ObjString *name = csStringCopy("toString", 8);
  for (ObjObject *at = AS_OBJECT(value); at != NULL; at = at->prototype) {
    Value found;
    if (csObjectGet(at, name, &found) && IS_CLOSURE(found)) {
      *method = found;
      return true;
    }
    if (at->klass == NULL) continue;
    ObjClosure *declared = csClassFindMethod(at->klass, name);
    if (declared != NULL) {
      *method = OBJ_VAL(declared);
      return true;
    }
  }
  return false;
}

/* Text for `value` the way a string conversion wants it: through the object's
 * own `toString` when it has one, and through the built-in rendering
 * otherwise. The caller owns the returned buffer, as with csValueToCString. */
char *csVMValueToText(Value value, size_t *length) {
  Value method;
  if (!findUserToString(value, &method)) return csValueToCString(value, length);

  csVMPush(value); /* the receiver a method call reads out of slot 0 */
  Value produced;
  if (!csVMCallCallback(method, 0, &produced)) return NULL;

  /* Whatever it answered with is then converted the ordinary way, so a
   * toString that returns a number is not a second special case. */
  return csValueToCString(produced, length);
}

/* A module's exports as one frozen object.
 *
 * The keys are sorted, because that is what the specification says a
 * namespace's keys are — the exporting file's declaration order is
 * deliberately not observable through one. Returns false with an error already
 * reported when the module exports more names than one can hold. */
static bool buildNamespace(ObjModule *module, Value *out) {
  /* One namespace object per module: `import * as a` and `await import(…)` of
   * the same file give the same object in JavaScript, and rebuilding it would
   * quietly make them different. */
  if (module->namespaceView != NULL) {
    *out = OBJ_VAL(module->namespaceView);
    return true;
  }

  /* Collected first so the sort does not have to happen against a hash
   * table. */
  ObjString *names[CS_NAMESPACE_MAX];
  int count = 0;
  for (int i = 0; i < module->exports.capacity; i++) {
    Entry *entry = &module->exports.entries[i];
    if (entry->key == NULL) continue;
    if (count == CS_NAMESPACE_MAX) {
      csVMRuntimeError("'%s' exports more than %d names", module->path->chars,
                       CS_NAMESPACE_MAX);
      return false;
    }
    names[count++] = entry->key;
  }

  /* Insertion sort: export lists are short, and this keeps the comparison —
   * code-unit order, shorter first on a prefix — in one readable place. */
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
  /* Kept on the stack while the copies below, which allocate, run. */
  csVMPush(OBJ_VAL(namespaceObject));

  for (int i = 0; i < count; i++) {
    Value value;
    if (!csTableGet(&module->globals, names[i], &value)) continue;
    csObjectPut(namespaceObject, names[i], value);
  }

  /* A namespace is a view of another module, not somewhere to put things. */
  csObjectFreeze(namespaceObject);
  csVMPop();
  module->namespaceView = namespaceObject;
  *out = OBJ_VAL(namespaceObject);
  return true;
}

/* An Error object with a formatted message, for something a program is meant
 * to catch rather than be stopped by. */
static Value csVMMakeError(const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);

  ObjObject *error = csObjectNew("Error");
  csPushTempRoot((Obj *)error);
  ObjString *name = csStringCopy("Error", 5);
  csPushTempRoot((Obj *)name);
  csObjectSetProperty(error, "name", OBJ_VAL(name));
  csPopTempRoot();
  ObjString *text = csStringCopy(message, (int)strlen(message));
  csPushTempRoot((Obj *)text);
  csObjectSetProperty(error, "message", OBJ_VAL(text));
  csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(error);
}

/* Loads, compiles and runs a module named at run time, and answers its
 * namespace. False with an error reported.
 *
 * The bodies run as nested calls rather than through csVMRunBody, which resets
 * the stack when it finishes — right for a top level and fatal in the middle
 * of one. */
static bool importAtRuntime(ObjModule *from, ObjString *specifier, Value *out,
                            bool *fatal) {
  *fatal = false;

  char resolved[4096];
  const char *fromPath = from != NULL ? from->path->chars : "<main>";
  if (!csModuleResolve(fromPath, specifier->chars, resolved, sizeof resolved)) {
    *out = csVMMakeError("cannot find module '%s'", specifier->chars);
    return false;
  }

  ObjModule *module = csModuleLoadResolved(resolved, specifier->chars, NULL, 0);
  if (module == NULL) {
    *out = csVMMakeError("cannot load module '%s'", specifier->chars);
    return false;
  }

  /* Everything the load queued, in dependency order — which for a module
   * already loaded is nothing at all, so importing one twice runs it once. */
  for (int i = 0; i < vm.pendingCount; i++) {
    ObjModule *pending = vm.pending[i];
    if (pending->executed) continue;

    ObjFunction *body = pending->body;
    pending->executed = true;
    pending->body = NULL;

    if (body->isAsync) {
      /* A top level that awaits has to settle before the namespace is worth
       * anything, and settling it means running the event loop from inside
       * this call. Naming that beats handing back a namespace of names that
       * are not there yet. */
      vm.pendingCount = 0;
      *out = csVMMakeError("'%s' has a top-level await, which import() cannot "
                           "wait for; import it statically instead",
                           csModuleDisplayPath(pending->path->chars));
      return false;
    }

    csPushTempRoot((Obj *)body);
    ObjClosure *closure = csClosureNew(body);
    csPopTempRoot();

    int baseFrame = vm.frameCount;
    csVMPush(OBJ_VAL(closure));
    if (!callClosure(closure, 0)) {
      vm.pendingCount = 0;
      *fatal = true;
      return false;
    }
    if (run(baseFrame) != CS_OK) {
      /* Something inside the module went wrong. A runtime error is not
       * catchable here, so it keeps travelling rather than becoming a
       * rejection that hides it. */
      vm.pendingCount = 0;
      *fatal = true;
      return false;
    }
    csVMPop(); /* what the body left behind */
  }
  vm.pendingCount = 0;

  return buildNamespace(module, out);
}

static bool concatenateOrAdd(void) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (IS_STRING(a) || IS_STRING(b)) {
    size_t aLength = 0;
    size_t bLength = 0;
    char *aText = csVMValueToText(a, &aLength);
    char *bText = csVMValueToText(b, &bLength);
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

  if (IS_BIGINT(a) || IS_BIGINT(b)) return bigintArithmetic('+', "+");

  if (!IS_NUMBER(a) || !IS_NUMBER(b)) {
    csVMRuntimeError("cannot add %s and %s", csValueTypeName(a), csValueTypeName(b));
    return false;
  }

  csVMPop();
  csVMPop();
  csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
  return true;
}

/* Slides the arguments a call supplied up and drops `bind`'s in front of them.
 *
 * The stack already holds [callee][supplied…], and the callee's own slot is
 * about to be overwritten with the receiver, so the presets go exactly where
 * the arguments a direct call would have pushed first belong. */
static bool spliceBoundArguments(ObjBoundMethod *bound, int *argCount) {
  if (bound->presets == NULL || bound->presets->elements.count == 0) return true;

  int extra = bound->presets->elements.count;
  if (vm.stackTop + extra >= vm.stack + vm.stackCapacity) {
    csVMRuntimeError("call stack overflow while applying a bound function");
    return false;
  }

  Value *supplied = vm.stackTop - *argCount;
  memmove(supplied + extra, supplied, sizeof(Value) * (size_t)*argCount);
  for (int i = 0; i < extra; i++) supplied[i] = bound->presets->elements.values[i];

  vm.stackTop += extra;
  *argCount += extra;
  return true;
}

/* Checks the argument count and pads the frame out to the parameter count.
 *
 * A parameter with a default is optional, so a call may supply fewer than
 * there are parameters — but the body still refers to every one of them by
 * slot, so the missing ones are pushed as undefined before the frame starts.
 * That is also what makes an argument written as `undefined` and one left out
 * the same thing, which is what JavaScript says. */
/* The contract at the gradual boundary.
 *
 * A `number` annotation is not advice: the compiler emits OP_ADD_NUM for
 * arithmetic on it and the JIT seeds the slot as a number and stops guarding.
 * The checker enforces the annotation wherever it can see the argument's type
 * — but `any` is assignable to everything by design, so a value that came in
 * through an untyped edge reaches the callee unexamined. Without this check it
 * would be read as a double it is not, and the answer would be quietly wrong
 * rather than a reported error.
 *
 * This is the one check type.h promised gradual typing would need. Only
 * `number` is checked, because `number` is the only annotation anything
 * currently generates code from — guarding a type nothing trusts would cost
 * without buying anything.
 *
 * Arguments the caller did not supply are not checked: those are about to be
 * padded with undefined and then filled in by the defaults prologue. */
static bool parameterTypesHold(ObjFunction *function, int argCount) {
  if (function->paramTypes == NULL) return true;

  int checked = argCount < function->paramCount ? argCount : function->paramCount;
  for (int i = 0; i < checked; i++) {
    if (function->paramTypes[i] != TYPE_NUMBER) continue;

    Value argument = vm.stackTop[i - argCount];
    if (IS_NUMBER(argument)) continue;

    const char *name =
        function->name != NULL ? function->name->chars : "<anonymous>";
    csVMRuntimeError("%s: argument %d is %s but the parameter is number", name,
                     i + 1, csValueTypeName(argument));
    return false;
  }
  return true;
}

bool csVMCheckArity(ObjFunction *function, int *argCount) {
  if (!parameterTypesHold(function, *argCount)) return false;

  /* A rest parameter takes whatever is left, so there is no upper bound and
   * the leftovers become one array in its slot. */
  if (function->hasRest) {
    int fixed = function->paramCount - 1;
    if (*argCount < function->arity) {
      const char *name =
          function->name != NULL ? function->name->chars : "<anonymous>";
      csVMRuntimeError("%s expects at least %d argument%s but got %d", name,
                       function->arity, function->arity == 1 ? "" : "s", *argCount);
      return false;
    }

    while (*argCount < fixed) {
      csVMPush(UNDEFINED_VAL);
      (*argCount)++;
    }

    ObjArray *rest = csArrayNew();
    csPushTempRoot((Obj *)rest);
    for (int i = fixed; i < *argCount; i++) {
      csValueArrayWrite(&rest->elements, vm.stackTop[i - *argCount]);
    }
    csPopTempRoot();

    vm.stackTop -= *argCount - fixed;
    csVMPush(OBJ_VAL(rest));
    *argCount = fixed + 1;
    return true;
  }

  if (*argCount < function->arity || *argCount > function->paramCount) {
    const char *name =
        function->name != NULL ? function->name->chars : "<anonymous>";
    if (function->arity == function->paramCount) {
      csVMRuntimeError("%s expects %d argument%s but got %d", name, function->arity,
                       function->arity == 1 ? "" : "s", *argCount);
    } else {
      csVMRuntimeError("%s expects between %d and %d arguments but got %d", name,
                       function->arity, function->paramCount, *argCount);
    }
    return false;
  }

  while (*argCount < function->paramCount) {
    csVMPush(UNDEFINED_VAL);
    (*argCount)++;
  }
  return true;
}

/* Pushes a frame for a user function. The frame's window starts at the callee
 * itself, so slot 0 is the function and slots 1..arity are the arguments —
 * exactly where the compiler assigned the parameters. */
bool callClosure(ObjClosure *closure, int argCount) {
  return csVMCallClosureWith(closure, argCount, false);
}

/* `hasReceiver` says slot 0 already holds what `this` should be — a method's
 * receiver, or the object `new` is building. Without it the slot still holds
 * the callee, and a function that says `this` would see itself. JavaScript
 * answers `undefined` there in a module, and so does this. */
/* Records what this call passed, for a function still being counted towards
 * the compile threshold.
 *
 * Only while it is being counted: once the decision is made the observation
 * has been used, and going on paying for it on every call would be a tax on
 * exactly the code the compiler is trying to speed up. A call that arrives
 * after that with a different type is caught by the entry guard instead. */
#ifdef CS_DEBUG_JIT
static void observeArguments(ObjFunction *function, int argCount) {
  if (function->paramCount == 0 || function->paramCount > 8) return;

  if (function->observedParams == NULL) {
    function->observedParams = (uint8_t *)calloc((size_t)function->paramCount, 1);
    if (function->observedParams == NULL) return;
  }

  for (int i = 0; i < function->paramCount; i++) {
    if (function->observedParams[i] == CS_PARAM_MIXED) continue;
    /* A call that did not supply this one says nothing about it: the defaults
     * prologue has not run yet, so the slot holds nothing worth reading. */
    if (i >= argCount) continue;

    bool isNumber = IS_NUMBER(vm.stackTop[i - argCount]);
    function->observedParams[i] = isNumber ? CS_PARAM_NUMBER : CS_PARAM_MIXED;
  }
}
#endif

bool csVMCallClosureWith(ObjClosure *closure, int argCount, bool hasReceiver) {
  ObjFunction *function = closure->function;

  /* A method is never called without one: every path that reaches one puts the
   * receiver in slot 0 first, including a native that pushes it by hand before
   * running a getter. Only a plain function can arrive here with the callee
   * still sitting in the slot its `this` will read. */
  if (function->usesThis && !function->isMethod && !hasReceiver) {
    vm.stackTop[-argCount - 1] = UNDEFINED_VAL;
  }

  if (!csVMCheckArity(function, &argCount)) return false;

  if (vm.frameCount == vm.frameCapacity) {
    csVMRuntimeError("call stack overflow (limit %d frames)", vm.frameCapacity);
    return false;
  }

  /* A generator's body never runs here: the call builds the handle. */
  if (function->isGenerator) return callGeneratorFunction(closure, argCount);

#ifdef CS_DEBUG_JIT
  /* Before the tick, so the call that crosses the threshold is counted in what
   * the compiler is about to look at. */
  if (function->jitState == JIT_INTERPRETED) observeArguments(function, argCount);
#endif
  CS_JIT_TICK(function);

  /* Compiled code is *not* entered here, and the reason is this function's
   * contract: it promises a frame has been pushed, and csVMRunBody,
   * csVMCallCallback and csVMCallCallbackWithReceiver all start an interpreter
   * loop immediately afterwards on the strength of that promise. Answering
   * with a value instead is the native protocol, and callValue is the level
   * that allows it — which is where the entry lives.
   *
   * There used to be one here as well. Nothing caught it because reaching it
   * needed a function whose parameters were typed *and* which was called back
   * from a native — a comparator passed to `sort`, say — and no test had one
   * until parameter types started being guessed rather than declared. */
  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  /* Taken rather than read: the next call is a plain one unless OP_NEW sets it
   * again, which is what makes a constructor's own calls answer undefined the
   * way JavaScript's do. */
  frame->newTarget = vm.pendingNewTarget;
  vm.pendingNewTarget = UNDEFINED_VAL;
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
    /* A generator is a generator whether or not it is async: the call builds
     * the handle either way, and `async` only changes what `next()` answers
     * with. Checked first, because the async path would run the body. */
    if (closure->function->isGenerator) return callGeneratorFunction(closure, argCount);
    if (closure->function->isAsync) return callAsyncFunction(closure, argCount);

#ifdef CS_DEBUG_JIT
    /* Where the lowered IR covers a whole function, it runs instead of the
     * bytecode — which is what checks the lowering against the golden suite.
     *
     * It belongs here rather than in callClosure. That function's contract is
     * that a frame has been pushed, and csVMRunBody and csVMCallCallback both
     * start an interpreter loop immediately afterwards on the strength of it —
     * so replacing the frame with a value there segfaults on the entry call.
     * Returning a value instead is the *native* protocol, and this is the
     * level that already allows it. */
    {
      /* The boundary check happens here too, and has to: this path answers the
       * call without pushing a frame, so it never reaches csVMCheckArity —
       * and compiled code trusts a `number` annotation completely. Without
       * this, a value carried in through `any` was read as a double it is not,
       * and the same program answered differently with the compiler on. */
      if (!parameterTypesHold(closure->function, argCount)) return false;

      Value lowered;
      if (csJitTryRun(closure->function, vm.stackTop - argCount, argCount, &lowered)) {
        vm.stackTop -= argCount + 1;
        csVMPush(lowered);
        return true;
      }
    }
#endif

    return callClosure(closure, argCount);
  }
  if (IS_NATIVE(callee)) return callNative(AS_NATIVE(callee), UNDEFINED_VAL, argCount);

  if (IS_BOUND_METHOD(callee)) {
    ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
    if (!spliceBoundArguments(bound, &argCount)) return false;
    if (bound->method->type == OBJ_NATIVE) {
      /* Natives already take a receiver, so a bound one is simply called with
       * the value it was bound to. */
      return callNative((ObjNative *)bound->method, bound->receiver, argCount);
    }
    /* The receiver goes back into slot 0, which is where the method's `this`
     * reads from — the same place OP_INVOKE would have left it. */
    vm.stackTop[-argCount - 1] = bound->receiver;
    return csVMCallClosureWith((ObjClosure *)bound->method, argCount, true);
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
  if (method->function->isGenerator) return callGeneratorFunction(method, argCount);
  if (method->function->isAsync) return callAsyncFunction(method, argCount);
  return csVMCallClosureWith(method, argCount, true);
}

/* Does a call through a property have to hand this closure the receiver?
 *
 * In JavaScript `this` comes from the call site, not from how the function was
 * written: `o.f()` passes `o` whatever `f` is. A function written as a method
 * always wants it; a plain function wants it exactly when its body says
 * `this`, which is what makes `Point.prototype.sum = function () { ... }` work.
 * A function that never mentions `this` cannot tell the difference, so nothing
 * else has to change. */
static bool wantsReceiver(ObjClosure *closure) {
  return closure->function->isMethod || closure->function->usesThis;
}

/* The method table a receiver's built-ins live in, or NULL when it has none. */
static Table *methodTableFor(Value receiver) {
  if (IS_ARRAY(receiver)) return &vm.arrayMethods;
  if (IS_STRING(receiver)) return &vm.stringMethods;
  if (IS_PROMISE(receiver)) return &vm.promiseMethods;
  if (IS_MAP(receiver)) return AS_MAP(receiver)->isWeak ? &vm.weakMethods
                                                         : &vm.mapMethods;
  if (IS_GENERATOR(receiver)) return &vm.generatorMethods;
  if (IS_NUMBER(receiver)) return &vm.numberMethods;
  /* A closure or a bound method; a native's own statics are looked at first,
   * so `Number.isInteger` still wins over `Number.call`. */
  if (IS_CLOSURE(receiver) || IS_BOUND_METHOD(receiver) || IS_NATIVE(receiver)) {
    return &vm.functionMethods;
  }
  if (IS_REGEX(receiver)) return &vm.regexMethods;
  if (IS_DATE(receiver)) return &vm.dateMethods;
  if (IS_SYMBOL(receiver)) return &vm.symbolMethods;
  if (IS_BIGINT(receiver)) return &vm.bigintMethods;
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
    /* `this.#run()` where `#run` is a field holding a function. Private fields
     * are not in the shape, so the ordinary lookup below would miss them and
     * fall through to the class's private methods. */
    if (name->length > 0 && name->chars[0] == '#' &&
        csObjectGetPrivate(AS_OBJECT(receiver), name, &method)) {
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }
    /* Own properties first, then the prototype chain — which is where a method
     * shared by every object built from the same prototype lives. */
    if (csObjectGetInherited(AS_OBJECT(receiver), name, &method)) {
      /* A property written as a method — `{ m() {} }` — keeps the receiver,
       * because that is what its `this` resolves to. Anything else is just a
       * function that happens to live on an object, and gets no receiver. */
      if (IS_CLOSURE(method) && wantsReceiver(AS_CLOSURE(method))) {
        return callMethod(AS_CLOSURE(method), argCount);
      }

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

/* The string a value becomes when used as an object key.
 *
 * JavaScript converts anything: `o[1]` and `o["1"]` are the same property.
 * Returns NULL only when the conversion could not allocate, in which case the
 * caller reports it. The result is unrooted, so a caller that allocates again
 * before storing it must root it first. */
static ObjString *objectKeyFor(Value key) {
  if (IS_STRING(key)) return AS_STRING(key);
  /* A symbol is filed under a name no source can write, which is what keeps a
   * symbol-keyed property out of everything that walks an object by name —
   * exactly where JavaScript puts one. */
  if (IS_SYMBOL(key)) return AS_SYMBOL(key)->key;

  size_t length = 0;
  char *text = csValueToCString(key, &length);
  if (text == NULL) return NULL;
  ObjString *converted = csStringCopy(text, (int)length);
  free(text);
  return converted;
}

/* Every symbol whose property this object carries.
 *
 * A symbol-keyed property is filed under a string, so the string has to be
 * mapped back — which is what `vm.symbolsByKey` is for. Nothing else needs it,
 * and it is weak in the sense that matters: it holds only symbols that some
 * object is already keyed by. */
void csVMCollectSymbolKeys(ObjObject *object, ObjArray *into) {
  if (object->privates == NULL) return;

  for (int i = 0; i < object->privates->capacity; i++) {
    ObjString *key = object->privates->entries[i].key;
    if (key == NULL) continue;

    Value symbol;
    if (!csTableGet(&vm.symbolsByKey, key, &symbol)) continue;
    csValueArrayWrite(&into->elements, symbol);
  }
}

/* The function an object offers as its `Symbol.iterator`, or NULL.
 *
 * Looked for where a symbol-keyed property lives — beside the shape — and then
 * on the class, so both `{ [Symbol.iterator]() {} }` and a class that declares
 * the method work. */
static bool findIteratorMethod(Value target, Value *out) {
  if (!IS_OBJECT(target) || vm.iteratorSymbol == NULL) return false;
  ObjObject *object = AS_OBJECT(target);

  if (csObjectGetPrivate(object, vm.iteratorSymbol->key, out)) return true;
  if (object->klass != NULL) {
    ObjClosure *method = csClassFindMethod(object->klass, vm.iteratorSymbol->key);
    if (method != NULL) {
      *out = OBJ_VAL(method);
      return true;
    }
  }
  return false;
}

/* The same, for `Symbol.asyncIterator`. `for await` asks for this one first
 * and falls back to the sync protocol, which is what JavaScript does. */
static bool findAsyncIteratorMethod(Value target, Value *out) {
  if (!IS_OBJECT(target) || vm.asyncIteratorSymbol == NULL) return false;
  ObjObject *object = AS_OBJECT(target);

  if (csObjectGetPrivate(object, vm.asyncIteratorSymbol->key, out)) return true;
  if (object->klass != NULL) {
    ObjClosure *method = csClassFindMethod(object->klass, vm.asyncIteratorSymbol->key);
    if (method != NULL) {
      *out = OBJ_VAL(method);
      return true;
    }
  }
  return false;
}

/* Calls `callee` with `receiver` in slot 0, for the places the VM itself has
 * to invoke a user method: the iterator protocol, so far. */
static bool callWithReceiver(Value callee, Value receiver, Value *args, int argCount,
                             Value *out) {
  if (!IS_CLOSURE(callee)) return csVMCallAdapted(callee, args, argCount, out);

  ObjBoundMethod *bound = csBoundMethodNew(receiver, AS_OBJ(callee));
  csPushTempRoot((Obj *)bound);
  bool ok = csVMCallAdapted(OBJ_VAL(bound), args, argCount, out);
  csPopTempRoot();
  return ok;
}

/* One step of a user-defined iterator: `it.next()`, unpacked.
 *
 * A generator is the common case and is already an iterator, so it is pulled
 * directly rather than through a property lookup and a call. */
static bool iteratorStep(Value iterator, Value *value, bool *done) {
  if (IS_GENERATOR(iterator)) {
    return csGeneratorNext(AS_GENERATOR(iterator), UNDEFINED_VAL, value, done);
  }

  Value next;
  if (!IS_OBJECT(iterator) ||
      (!csObjectGet(AS_OBJECT(iterator), csStringCopy("next", 4), &next) &&
       (AS_OBJECT(iterator)->klass == NULL ||
        (next = OBJ_VAL(csClassFindMethod(AS_OBJECT(iterator)->klass,
                                          csStringCopy("next", 4))),
         !IS_CLOSURE(next))))) {
    csVMRuntimeError("an iterator must have a 'next' method");
    return false;
  }

  Value record;
  if (!callWithReceiver(next, iterator, NULL, 0, &record)) return false;
  if (!IS_OBJECT(record)) {
    csVMRuntimeError("'next' must answer with an object, got %s",
                     csValueTypeName(record));
    return false;
  }

  Value flag;
  *done = csObjectGet(AS_OBJECT(record), csStringCopy("done", 4), &flag) &&
          csValueIsTruthy(flag);
  if (!csObjectGet(AS_OBJECT(record), csStringCopy("value", 5), value)) {
    *value = UNDEFINED_VAL;
  }
  return true;
}

/* Whether `invokeMethod` would find something to call.
 *
 * Deliberately the same walk in the same order, because an answer that
 * disagreed with the call that follows it would turn `o.m?.()` into either a
 * skipped call or a runtime error. */
static bool receiverHasMethod(Value receiver, ObjString *name) {
  Value found;

  if (IS_OBJECT(receiver)) {
    ObjObject *instance = AS_OBJECT(receiver);
    Value ignored;
    if (name->length > 0 && name->chars[0] == '#' &&
        csObjectGetPrivate(instance, name, &ignored)) {
      return true;
    }
    if (csObjectGetInherited(instance, name, &found)) return true;
    return instance->klass != NULL &&
           csClassFindMethod(instance->klass, name) != NULL;
  }

  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      if (csTableGet(&klass->statics, name, &found)) return true;
      if (csTableGet(&klass->staticGetters, name, &found)) return true;
    }
    return false;
  }

  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    return csObjectGet(AS_NATIVE(receiver)->statics, name, &found);
  }

  Table *methods = methodTableFor(receiver);
  return methods != NULL && csTableGet(methods, name, &found);
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
/* `__proto__` is a link, not a property. Recognising it by name is what makes
 * `{ __proto__: base }` and `o.__proto__ = base` work without the accessor
 * machinery JavaScript defines it with — and it is why an object cannot have
 * an ordinary property under that name, which the docs record. */
static bool isProtoKey(const ObjString *name) {
  return name->length == 9 && memcmp(name->chars, "__proto__", 9) == 0;
}

/* A method that is *not* an own property travels with the object it was read
 * from — a class method, or one found on the prototype chain.
 *
 * The rule is drawn there rather than at "every method" because an own method
 * lives in the object's shape, and so is served by the inline cache: binding
 * it would mean testing every property read in the VM for whether it happened
 * to produce a method. A class method and an inherited one can never be in the
 * receiver's own shape, so they always reach this slow path and binding them
 * costs nothing anyone else pays.
 *
 * Only a closure written *as* a method is bound. A plain function that happens
 * to be stored on an object is just a function, and giving it a receiver it
 * never asked for would be inventing meaning for its `this`. */
static Value bindIfMethod(Value receiver, Value found) {
  if (IS_CLOSURE(found) && AS_CLOSURE(found)->function->isMethod) {
    return OBJ_VAL(csBoundMethodNew(receiver, (Obj *)AS_CLOSURE(found)));
  }
  return found;
}

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
    /* An own accessor's slot holds a stand-in rather than a value, so finding
     * it means the answer is further down, where accessors are looked for. */
    if (csObjectGet(object, name, out) && !csVMIsAccessorSlot(*out)) return true;

    /* `constructor` names whatever built the object: its class, or the
     * function `new` was applied to. Neither is an ordinary property — the
     * first is not stored at all and the second lives beside the shape — so
     * both are answered here rather than found by the walk below. */
    if (name->length == 11 && memcmp(name->chars, "constructor", 11) == 0) {
      for (ObjObject *at = object; at != NULL; at = at->prototype) {
        if (at->klass != NULL && !at->klass->isAccessorHolder) {
          *out = OBJ_VAL(at->klass);
          return true;
        }
        Value owner;
        if (csObjectGetPrivate(at, csStringCopy("#constructor", 12), &owner)) {
          *out = owner;
          return true;
        }
      }
      *out = UNDEFINED_VAL;
      return true;
    }

    /* `__proto__` reads the link itself rather than a property, which is the
     * only way to reach it without going through Object.getPrototypeOf. */
    if (isProtoKey(name)) {
      *out = object->prototype != NULL ? OBJ_VAL(object->prototype) : NULL_VAL;
      return true;
    }

    /* Everything else the object might answer with, at each link of the chain
     * in turn: a data property, an accessor, a method.
     *
     * An accessor lives on a class rather than in the shape — a synthetic one
     * per object for a literal's `get x()`, the real one for an instance — so
     * asking each link the same three questions is what makes an inherited
     * getter work at all. Doing it level by level is also what gives shadowing
     * the right answer: a nearer link's data property beats a further link's
     * getter, exactly as in JavaScript. */
    for (ObjObject *at = object; at != NULL; at = at->prototype) {
      /* The object's own data properties were checked above. */
      if (at != object && csObjectGet(at, name, out) &&
          !csVMIsAccessorSlot(*out)) {
        /* Bound to the object it was reached *through*, not the prototype it
         * was found on — that is what makes `this` mean the instance. */
        *out = bindIfMethod(receiver, *out);
        return true;
      }
      if (at->klass == NULL) continue;

      ObjClosure *getter = csClassFindGetter(at->klass, name);
      if (getter != NULL) {
        /* A getter is a call, so it runs a nested loop. It never enters the
         * shape, which is why the inline caches never see one. The receiver is
         * the object asked, not the one that turned out to define it. */
        csVMPush(receiver);
        return csVMCallCallback(OBJ_VAL(getter), 0, out);
      }

      ObjClosure *method = csClassFindMethod(at->klass, name);
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

  /* A pattern's own properties. Intrinsic for the same reason `.length` is:
   * the object does not carry a table. `lastIndex` is the only writable one,
   * handled beside this in the write path. */
  if (IS_REGEX(receiver)) {
    ObjRegex *regex = AS_REGEX(receiver);
    if (name->length == 9 && memcmp(name->chars, "lastIndex", 9) == 0) {
      *out = NUMBER_VAL(regex->lastIndex);
      return true;
    }
    if (name->length == 6 && memcmp(name->chars, "source", 6) == 0) {
      *out = OBJ_VAL(regex->source);
      return true;
    }
    if (name->length == 5 && memcmp(name->chars, "flags", 5) == 0) {
      *out = OBJ_VAL(regex->flags);
      return true;
    }
    if (name->length == 6 && memcmp(name->chars, "global", 6) == 0) {
      *out = BOOL_VAL(regex->global);
      return true;
    }
  }

  /* `.size` on a Map or Set, like `.length` on an array: intrinsic rather than
   * stored, so the collection does not have to carry a property table. */
  if (IS_MAP(receiver) && name->length == 4 && memcmp(name->chars, "size", 4) == 0) {
    /* A weak collection has no size to report: the answer would depend on
     * when the collector last ran, which is the one thing a program must not
     * be able to find out. */
    if (AS_MAP(receiver)->isWeak) {
      csVMRuntimeError("a %s has no size: how many entries are left depends on "
                       "when the collector last ran",
                       AS_MAP(receiver)->isSet ? "WeakSet" : "WeakMap");
      return false;
    }
    *out = NUMBER_VAL(AS_MAP(receiver)->liveCount);
    return true;
  }

  /* Named properties on an array — only `exec` results have any. Checked
   * before `length`, which no caller can shadow anyway, and after nothing:
   * an ordinary array has no table and this is one NULL test. */
  if (IS_ARRAY(receiver)) {
    Value extra;
    if (csArrayGetExtra(AS_ARRAY(receiver), name, &extra)) {
      *out = extra;
      return true;
    }
  }

  /* `F.prototype` — the object everything `new F()` builds inherits from. It
   * is created on the first ask rather than with the closure, because most
   * functions are never constructed from. */
  if (IS_CLOSURE(receiver) && name->length == 9 &&
      memcmp(name->chars, "prototype", 9) == 0) {
    *out = OBJ_VAL(csClosurePrototype(AS_CLOSURE(receiver)));
    return true;
  }

  /* `length` is intrinsic rather than a stored property, so arrays and strings
   * answer it without carrying a table. A function's is how many arguments it
   * must be given, which is the same number its arity check uses. */
  if (name->length == 6 && memcmp(name->chars, "length", 6) == 0) {
    if (IS_ARRAY(receiver)) {
      *out = NUMBER_VAL(AS_ARRAY(receiver)->elements.count);
      return true;
    }
    if (IS_STRING(receiver)) {
      *out = NUMBER_VAL(AS_STRING(receiver)->length);
      return true;
    }
    if (IS_CLOSURE(receiver)) {
      *out = NUMBER_VAL(AS_CLOSURE(receiver)->function->arity);
      return true;
    }
    if (IS_NATIVE(receiver)) {
      int arity = AS_NATIVE(receiver)->arity;
      *out = NUMBER_VAL(arity < 0 ? 0 : arity);
      return true;
    }
  }

  /* A symbol's description, which is the only thing about one that is not its
   * identity. */
  if (IS_SYMBOL(receiver) && name->length == 11 &&
      memcmp(name->chars, "description", 11) == 0) {
    ObjString *described = AS_SYMBOL(receiver)->description;
    *out = described != NULL ? OBJ_VAL(described) : UNDEFINED_VAL;
    return true;
  }

  /* And `name`, which a stack trace and a program should agree about. */
  if (name->length == 4 && memcmp(name->chars, "name", 4) == 0) {
    if (IS_CLOSURE(receiver)) {
      ObjString *given = AS_CLOSURE(receiver)->function->name;
      *out = OBJ_VAL(given != NULL ? given : csStringCopy("", 0));
      return true;
    }
    if (IS_NATIVE(receiver)) {
      *out = OBJ_VAL(csStringCopy(AS_NATIVE(receiver)->name->chars,
                                  AS_NATIVE(receiver)->name->length));
      return true;
    }
    if (IS_CLASS(receiver)) {
      *out = OBJ_VAL(AS_CLASS(receiver)->name);
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
      /* A static getter is run rather than handed back, and `this` inside it
       * is the class it was asked of. */
      Value getter;
      if (csTableGet(&klass->staticGetters, name, &getter)) {
        csVMPush(receiver);
        return csVMCallCallback(getter, 0, out);
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
  /* `lastIndex` is writable, which is how a `g` pattern is rewound. */
  if (IS_REGEX(receiver) && name->length == 9 &&
      memcmp(name->chars, "lastIndex", 9) == 0) {
    if (!IS_NUMBER(value)) {
      csVMRuntimeError("lastIndex must be a number");
      return false;
    }
    AS_REGEX(receiver)->lastIndex = (int)AS_NUMBER(value);
    return true;
  }

  /* A class carries its statics, so assigning to one is how a counter kept on
   * the class is updated. It never enters a shape, so it never caches. */
  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value setter;
      if (csTableGet(&klass->staticSetters, name, &setter)) {
        csVMPush(receiver);
        csVMPush(value);
        Value ignored;
        return csVMCallCallback(setter, 1, &ignored);
      }
      Value getter;
      if (csTableGet(&klass->staticGetters, name, &getter)) {
        csVMRuntimeError("'%s' has only a getter and cannot be assigned to",
                         name->chars);
        return false;
      }
    }
    csTableSet(&AS_CLASS(receiver)->statics, name, value);
    return true;
  }

  /* Replacing a function's prototype wholesale, which is how the pattern
   * `F.prototype = { m() {} }` is written. */
  if (IS_CLOSURE(receiver) && name->length == 9 &&
      memcmp(name->chars, "prototype", 9) == 0) {
    if (!IS_OBJECT(value)) {
      csVMRuntimeError("a prototype must be an object, got %s",
                       csValueTypeName(value));
      return false;
    }
    AS_CLOSURE(receiver)->prototype = AS_OBJECT(value);
    return true;
  }

  if (!IS_OBJECT(receiver)) {
    csVMRuntimeError("cannot set property '%s' of %s", name->chars,
                     csValueTypeName(receiver));
    return false;
  }
  if (AS_OBJECT(receiver)->frozen) {
    csVMRuntimeError("'%s.%s' cannot be changed: the object is frozen",
                     AS_OBJECT(receiver)->name->chars, name->chars);
    return false;
  }

  /* Assigning to `__proto__` re-links the object rather than storing anything,
   * which is what makes it the same link Object.setPrototypeOf sets. */
  if (isProtoKey(name)) {
    ObjObject *object = AS_OBJECT(receiver);
    if (IS_NULL(value) || IS_UNDEFINED(value)) {
      object->prototype = NULL;
      return true;
    }
    if (!IS_OBJECT(value)) {
      /* JavaScript ignores a non-object here rather than complaining, which
       * turns a mistake into a link that silently did not happen. */
      csVMRuntimeError("a prototype must be an object or null, got %s",
                       csValueTypeName(value));
      return false;
    }
    if (!csObjectSetPrototype(object, AS_OBJECT(value))) {
      csVMRuntimeError("that prototype is already in this object's chain, "
                       "which would make every lookup on it loop");
      return false;
    }
    return true;
  }

  /* A setter takes the write instead of the object storing it. Checked before
   * the store, so the property never enters the shape and the caches stay out
   * of it — and looked for up the whole chain, because a setter inherited from
   * a prototype is still the thing that owns this name. */
  for (ObjObject *at = AS_OBJECT(receiver); at != NULL; at = at->prototype) {
    if (at->klass == NULL) continue;
    ObjClosure *setter = csClassFindSetter(at->klass, name);
    if (setter != NULL) {
      csVMPush(receiver);
      csVMPush(value);
      Value ignored;
      return csVMCallCallback(OBJ_VAL(setter), 1, &ignored);
    }
    /* A getter with no setter is read-only, and silently dropping the write is
     * how a bug hides. */
    if (csClassFindGetter(at->klass, name) != NULL) {
      csVMRuntimeError("'%s' has only a getter and cannot be assigned to",
                       name->chars);
      return false;
    }
  }

  /* A property `Object.defineProperty` marked read-only refuses the write
   * rather than dropping it: a store that silently does nothing is how a bug
   * hides, and an ES module throws here too. The object left shape mode when
   * the property was defined, so the write fast path never reaches this
   * property and the check costs nothing anywhere else. */
  if ((csObjectAttributes(AS_OBJECT(receiver), name) & CS_PROP_WRITABLE) == 0) {
    csVMRuntimeError("'%s' is read-only and cannot be assigned to", name->chars);
    return false;
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

/* The same, but a BigInt on either side takes the exact path instead. `code`
 * is what bigintArithmetic() switches on; it differs from the operator only
 * for `**`, which is not a character. */
#define BINARY_ARITHMETIC_OP(op, code)                                      \
  do {                                                                      \
    if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {               \
      if (!bigintArithmetic(code, #op)) return CS_RUNTIME_ERROR;            \
      break;                                                                \
    }                                                                       \
    BINARY_NUMERIC_OP(NUMBER_VAL, op);                                      \
  } while (false)

/* Ordering, where a BigInt and a number do mix. */
#define BINARY_COMPARE_OP(op)                                               \
  do {                                                                      \
    if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {               \
      int order = 0;                                                        \
      OrderResult ordering = comparingExactly(peekStack(1), peekStack(0),   \
                                              &order);                      \
      if (ordering == ORDER_INVALID) return CS_RUNTIME_ERROR;               \
      vm.stackTop -= 2;                                                     \
      /* Unordered means NaN, and every comparison with NaN is false —      \
       * including `>=`, which is why this is not `!(...)`. */              \
      csVMPush(BOOL_VAL(ordering == ORDER_KNOWN && (order op 0)));          \
      break;                                                                \
    }                                                                       \
    BINARY_NUMERIC_OP(BOOL_VAL, op);                                        \
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

      /* Both exist for reading a target that is also being written: `o.x += 1`
       * needs `o` on the stack twice, and `xs[i] += 1` needs `xs` and `i`. */
      VM_CASE(OP_DUP2) {
        Value under = peekStack(1);
        Value top = peekStack(0);
        csVMPush(under);
        csVMPush(top);
        VM_NEXT();
      }
      VM_CASE(OP_POP_UNDER) {
        Value top = csVMPop();
        csVMPop();
        csVMPush(top);
        VM_NEXT();
      }

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
          /* A symbol names a property that nothing walking the object by name
           * can reach, so it is kept beside the shape rather than in it. */
          if (IS_SYMBOL(index)) {
            Value found;
            bool has = csObjectGetPrivate(AS_OBJECT(target), AS_SYMBOL(index)->key,
                                          &found);
            vm.stackTop -= 2;
            csVMPush(has ? found : UNDEFINED_VAL);
            VM_NEXT();
          }

          /* `o[1]` and `o["1"]` name the same property, so the key converts
           * rather than being rejected — which is also what `{ 1: x }` had to
           * store under for the two to meet. */
          ObjString *key = objectKeyFor(index);
          if (key == NULL) {
            csVMRuntimeError("out of memory converting an object key");
            return CS_RUNTIME_ERROR;
          }
          Value value;
          bool found = csObjectGetInherited(AS_OBJECT(target), key, &value);
          vm.stackTop -= 2;
          csVMPush(found ? value : UNDEFINED_VAL);
          VM_NEXT();
        }

        csVMRuntimeError("cannot index %s", csValueTypeName(target));
        return CS_RUNTIME_ERROR;
      }

      VM_CASE(OP_REGEX) {
        ObjString *source = READ_STRING();
        ObjString *flags = READ_STRING();
        ObjRegex *regex = csRegexObjectNew(source, flags);
        if (regex == NULL) return CS_RUNTIME_ERROR;
        csVMPush(OBJ_VAL(regex));
        VM_NEXT();
      }

      VM_CASE(OP_ITER_PREPARE) {
        uint8_t forAwait = READ_BYTE();
        /* Arrays and strings are already indexable, so the common case is a
         * type test and nothing else. */
        Value target = peekStack(0);

        /* An object that offers `Symbol.iterator` is replaced by whatever that
         * hands back, and the loop pulls from there — which is the whole of
         * what makes a user-defined type iterable. */
        Value iteratorMethod;
        if ((forAwait && findAsyncIteratorMethod(target, &iteratorMethod)) ||
            findIteratorMethod(target, &iteratorMethod)) {
          Value iterator;
          if (!callWithReceiver(iteratorMethod, target, NULL, 0, &iterator)) {
            return CS_RUNTIME_ERROR;
          }
          csVMPop();
          csVMPush(iterator);
          frame = &vm.frames[vm.frameCount - 1];
          VM_NEXT();
        }

        if (!IS_MAP(target)) VM_NEXT();
        if (AS_MAP(target)->isWeak) {
          csVMRuntimeError("a %s cannot be iterated: what is left in it depends "
                           "on when the collector last ran",
                           AS_MAP(target)->isSet ? "WeakSet" : "WeakMap");
          return CS_RUNTIME_ERROR;
        }

        ObjArray *items = csMapToArray(AS_MAP(target));
        csVMPop();
        csVMPush(OBJ_VAL(items));
        VM_NEXT();
      }

      VM_CASE(OP_ENUM_KEYS) {
        Value target = peekStack(0);
        ObjArray *keys = csArrayNew();
        csVMPush(OBJ_VAL(keys)); /* rooted while the copies below allocate */

        if (IS_OBJECT(target)) {
          ObjObject *object = AS_OBJECT(target);
          for (int i = 0; i < csObjectCount(object); i++) {
            ObjString *key = csObjectKeyAt(object, i);
            if (!csObjectIsEnumerable(object, key)) continue;
            csValueArrayWrite(&keys->elements, OBJ_VAL(key));
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

      VM_CASE(OP_JUMP_IF_ASYNC_ITER) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        /* An async generator, or any iterator object — the second may answer
         * `next()` with a promise or with a record, and the await that follows
         * handles both. */
        if ((IS_GENERATOR(top) && AS_GENERATOR(top)->isAsync) || IS_OBJECT(top)) {
          frame->ip += offset;
        }
        VM_NEXT();
      }

      VM_CASE(OP_ASYNC_NEXT) {
        Value target = csVMPop();
        if (IS_GENERATOR(target)) {
          csVMPush(OBJ_VAL(csGeneratorNextAsync(AS_GENERATOR(target), UNDEFINED_VAL)));
          VM_NEXT();
        }

        Value next;
        if (!IS_OBJECT(target) ||
            !csObjectGet(AS_OBJECT(target), csStringCopy("next", 4), &next)) {
          if (!IS_OBJECT(target) || AS_OBJECT(target)->klass == NULL ||
              (next = OBJ_VAL(csClassFindMethod(AS_OBJECT(target)->klass,
                                                csStringCopy("next", 4))),
               !IS_CLOSURE(next))) {
            csVMRuntimeError("an async iterator must have a 'next' method");
            return CS_RUNTIME_ERROR;
          }
        }

        Value answered;
        if (!callWithReceiver(next, target, NULL, 0, &answered)) {
          return CS_RUNTIME_ERROR;
        }
        csVMPush(answered);
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_ITER_UNPACK) {
        uint16_t offset = READ_SHORT();
        Value record = peekStack(0);
        if (!IS_OBJECT(record)) {
          csVMRuntimeError("an async iterator must answer with an object, got %s",
                           csValueTypeName(record));
          return CS_RUNTIME_ERROR;
        }

        Value done, produced;
        ObjObject *object = AS_OBJECT(record);
        if (!csObjectGet(object, csStringCopy("done", 4), &done)) done = BOOL_VAL(true);
        if (!csObjectGet(object, csStringCopy("value", 5), &produced)) {
          produced = UNDEFINED_VAL;
        }

        csVMPop();
        if (csValueIsTruthy(done)) {
          frame->ip += offset;
          VM_NEXT();
        }
        csVMPush(produced);
        VM_NEXT();
      }

      VM_CASE(OP_DESTRUCTURE_PREPARE) {
        uint8_t wanted = READ_BYTE();
        Value target = peekStack(0);
        if (IS_ARRAY(target) || IS_STRING(target)) VM_NEXT();

        /* A Map or a Set walks to what iterating it yields, the same
         * conversion `for...of` and a spread both make. */
        if (IS_MAP(target)) {
          if (AS_MAP(target)->isWeak) {
            csVMRuntimeError("a %s cannot be destructured: what is left in it "
                             "depends on when the collector last ran",
                             AS_MAP(target)->isSet ? "WeakSet" : "WeakMap");
            return CS_RUNTIME_ERROR;
          }
          ObjArray *items = csMapToArray(AS_MAP(target));
          csVMPop();
          csVMPush(OBJ_VAL(items));
          VM_NEXT();
        }

        Value source = target;
        if (!IS_GENERATOR(source)) {
          Value iteratorMethod;
          if (!findIteratorMethod(source, &iteratorMethod)) VM_NEXT();
          if (!callWithReceiver(iteratorMethod, source, NULL, 0, &source)) {
            return CS_RUNTIME_ERROR;
          }
        }

        csPushTempRoot(AS_OBJ(source));
        ObjArray *taken = csArrayNew();
        csPushTempRoot((Obj *)taken);
        for (int i = 0; wanted == 255 || i < wanted; i++) {
          Value produced;
          bool done;
          if (!iteratorStep(source, &produced, &done)) {
            csPopTempRoot();
            csPopTempRoot();
            return CS_RUNTIME_ERROR;
          }
          if (done) break;
          csValueArrayWrite(&taken->elements, produced);
        }
        csPopTempRoot();
        csPopTempRoot();

        csVMPop();
        csVMPush(OBJ_VAL(taken));
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_ITER_STEP) {
        uint16_t offset = READ_SHORT();
        Value index = peekStack(0);
        Value iterable = peekStack(1);

        if (IS_ARRAY(iterable)) {
          /* The length is read each pass, so a body that appends is seen —
           * which is what the index-driven loop did before. */
          ObjArray *array = AS_ARRAY(iterable);
          int at = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (at >= array->elements.count) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(array->elements.values[at]);
          VM_NEXT();
        }

        if (IS_STRING(iterable)) {
          ObjString *string = AS_STRING(iterable);
          int at = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (at >= string->length) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(OBJ_VAL(csStringCopy(string->chars + at, 1)));
          VM_NEXT();
        }

        if (IS_GENERATOR(iterable)) {
          ObjGenerator *generator = AS_GENERATOR(iterable);
          Value produced;
          bool done;
          /* Resuming runs a nested interpreter on the generator's own stack.
           * It restores this one before returning, so `frame` still points
           * where it did. */
          if (!csGeneratorNext(generator, UNDEFINED_VAL, &produced, &done)) {
            return CS_RUNTIME_ERROR;
          }
          vm.stackTop -= 2;
          if (done) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(produced);
          VM_NEXT();
        }

        /* Anything left is either an iterator OP_ITER_PREPARE produced, or a
         * value that was never iterable. */
        if (IS_OBJECT(iterable)) {
          Value produced;
          bool done;
          if (!iteratorStep(iterable, &produced, &done)) return CS_RUNTIME_ERROR;
          vm.stackTop -= 2;
          frame = &vm.frames[vm.frameCount - 1];
          if (done) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(produced);
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
        } else if (IS_OBJECT(target) && IS_SYMBOL(index)) {
          csObjectPutPrivate(AS_OBJECT(target), AS_SYMBOL(index)->key, value);
        } else if (IS_OBJECT(target)) {
          ObjString *key = objectKeyFor(index);
          if (key == NULL) {
            csVMRuntimeError("out of memory converting an object key");
            return CS_RUNTIME_ERROR;
          }
          csPushTempRoot((Obj *)key); /* csObjectPut can collect */
          /* `console["log"] = f` is the same act as `console.log = f`. */
          if (AS_OBJECT(target)->frozen) {
            csPopTempRoot();
            csVMRuntimeError("'%s.%s' cannot be changed: the object is frozen",
                             AS_OBJECT(target)->name->chars, key->chars);
            return CS_RUNTIME_ERROR;
          }
          csObjectPut(AS_OBJECT(target), key, value);
          csPopTempRoot();
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
          if (IS_SYMBOL(key)) {
            csObjectPutPrivate(object, AS_SYMBOL(key)->key, entries[i * 2 + 1]);
            continue;
          }
          if (!IS_STRING(key)) {
            ObjString *converted = objectKeyFor(key);
            if (converted == NULL) {
              csVMRuntimeError("out of memory building an object key");
              return CS_RUNTIME_ERROR;
            }
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
        /* A Map or Set spreads to what iterating it yields, which is the same
         * conversion `for...of` makes — they have to agree. */
        if (IS_MAP(peekStack(0)) && AS_MAP(peekStack(0))->isWeak) {
          csVMRuntimeError("a %s cannot be spread: what is left in it depends on "
                           "when the collector last ran",
                           AS_MAP(peekStack(0))->isSet ? "WeakSet" : "WeakMap");
          return CS_RUNTIME_ERROR;
        }
        if (IS_MAP(peekStack(0))) {
          ObjArray *items = csMapToArray(AS_MAP(peekStack(0)));
          csVMPop();
          csVMPush(OBJ_VAL(items));
        }

        /* An object that offers `Symbol.iterator` spreads to what it yields,
         * which is the same conversion `for...of` makes — the two have to
         * agree about what a value contains. */
        {
          Value iteratorMethod;
          if (findIteratorMethod(peekStack(0), &iteratorMethod)) {
            Value iterator;
            if (!callWithReceiver(iteratorMethod, peekStack(0), NULL, 0, &iterator)) {
              return CS_RUNTIME_ERROR;
            }
            csPushTempRoot(AS_OBJ(iterator));

            ObjArray *drained = csArrayNew();
            csPushTempRoot((Obj *)drained);
            for (;;) {
              Value produced;
              bool done;
              if (!iteratorStep(iterator, &produced, &done)) {
                csPopTempRoot();
                csPopTempRoot();
                return CS_RUNTIME_ERROR;
              }
              if (done) break;
              csValueArrayWrite(&drained->elements, produced);
            }
            csPopTempRoot();
            csPopTempRoot();

            csVMPop();
            csVMPush(OBJ_VAL(drained));
            frame = &vm.frames[vm.frameCount - 1];
          }
        }

        /* A generator spreads to everything it has left. Spreading is eager by
         * definition — the array has to be complete before it exists — so
         * running the body to its end here is not a shortcut, it is what the
         * operation means. An endless generator does not terminate, which is
         * equally true in JavaScript. */
        if (IS_GENERATOR(peekStack(0))) {
          ObjGenerator *generator = AS_GENERATOR(peekStack(0));
          ObjArray *drained = csArrayNew();
          csPushTempRoot((Obj *)drained);
          for (;;) {
            Value produced;
            bool done;
            if (!csGeneratorNext(generator, UNDEFINED_VAL, &produced, &done)) {
              csPopTempRoot();
              return CS_RUNTIME_ERROR;
            }
            if (done) break;
            csValueArrayWrite(&drained->elements, produced);
          }
          csPopTempRoot();
          csVMPop();
          csVMPush(OBJ_VAL(drained));
        }

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

      VM_CASE(OP_GET_PRIVATE) {
        ObjString *name = READ_STRING();
        Value target = csVMPop();
        Value value;
        /* `static #n` belongs to the class, where the statics already live —
         * and a name with a hash in it is no more reachable there. */
        if (IS_CLASS(target)) {
          csVMPush(csTableGet(&AS_CLASS(target)->statics, name, &value)
                       ? value
                       : UNDEFINED_VAL);
          VM_NEXT();
        }
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot read %s of %s", name->chars,
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(csObjectGetPrivate(AS_OBJECT(target), name, &value)
                     ? value
                     : UNDEFINED_VAL);
        VM_NEXT();
      }

      VM_CASE(OP_SET_PRIVATE) {
        ObjString *name = READ_STRING();
        Value value = peekStack(0);
        Value target = peekStack(1);
        if (IS_CLASS(target)) {
          csTableSet(&AS_CLASS(target)->statics, name, value);
          vm.stackTop -= 2;
          csVMPush(value);
          VM_NEXT();
        }
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot write %s of %s", name->chars,
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        csObjectPutPrivate(AS_OBJECT(target), name, value);
        vm.stackTop -= 2;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_DELETE_PROPERTY) {
        ObjString *name = READ_STRING();
        Value target = csVMPop();
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot delete a property of %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        if (AS_OBJECT(target)->frozen) {
          csVMRuntimeError("'%s' is frozen, so its properties cannot be deleted",
                           AS_OBJECT(target)->name->chars);
          return CS_RUNTIME_ERROR;
        }
        if ((csObjectAttributes(AS_OBJECT(target), name) &
             CS_PROP_CONFIGURABLE) == 0) {
          csVMRuntimeError("'%s' is not configurable and cannot be deleted",
                           name->chars);
          return CS_RUNTIME_ERROR;
        }
        /* JavaScript answers true when the property was not there either —
         * false is reserved for one it refused to remove, and a frozen
         * built-in is reported above rather than answered. */
        csObjectDelete(AS_OBJECT(target), name);
        csVMPush(BOOL_VAL(true));
        VM_NEXT();
      }

      VM_CASE(OP_DELETE_INDEX) {
        Value index = peekStack(0);
        Value target = peekStack(1);
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot delete a property of %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        if (AS_OBJECT(target)->frozen) {
          csVMRuntimeError("'%s' is frozen, so its properties cannot be deleted",
                           AS_OBJECT(target)->name->chars);
          return CS_RUNTIME_ERROR;
        }

        if (IS_SYMBOL(index)) {
          csObjectDeletePrivate(AS_OBJECT(target), AS_SYMBOL(index)->key);
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(true));
          VM_NEXT();
        }

        ObjString *key = objectKeyFor(index);
        if (key == NULL) {
          csVMRuntimeError("out of memory converting an object key");
          return CS_RUNTIME_ERROR;
        }
        csObjectDelete(AS_OBJECT(target), key);
        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(true));
        VM_NEXT();
      }

      VM_CASE(OP_INVOKE_INDEX) {
        /* `o[key](args...)`, with the stack holding [receiver][key][args…].
         *
         * A computed callee is not always a method: it may be an element of an
         * array of functions, or a symbol-keyed method, neither of which the
         * by-name dispatch below would find. Those are read first, exactly as
         * `o[key]` reads them; everything else goes to the same dispatch a
         * dotted call takes, which is what makes `"abc"["toUpperCase"]()`
         * reach the built-in method tables. */
        int argCount = READ_BYTE();
        Value keyValue = vm.stackTop[-argCount - 1];
        Value receiver = vm.stackTop[-argCount - 2];

        Value found;
        bool haveValue = false;
        if (IS_SYMBOL(keyValue) && IS_OBJECT(receiver)) {
          haveValue = csObjectGetPrivate(AS_OBJECT(receiver),
                                         AS_SYMBOL(keyValue)->key, &found);
        } else if (IS_ARRAY(receiver) && IS_NUMBER(keyValue)) {
          ObjArray *array = AS_ARRAY(receiver);
          int index = (int)AS_NUMBER(keyValue);
          haveValue = index >= 0 && index < array->elements.count;
          if (haveValue) found = array->elements.values[index];
        }

        ObjString *name = NULL;
        if (!haveValue) {
          name = IS_SYMBOL(keyValue) ? AS_SYMBOL(keyValue)->key
                                     : objectKeyFor(keyValue);
          if (name == NULL) {
            csVMRuntimeError("out of memory converting a method name");
            return CS_RUNTIME_ERROR;
          }
        }
        if (name != NULL) csPushTempRoot((Obj *)name);

        /* Collapsing the key out leaves exactly the layout OP_INVOKE builds by
         * hand, so both paths below see the stack they expect. */
        memmove(&vm.stackTop[-argCount - 1], &vm.stackTop[-argCount],
                sizeof(Value) * (size_t)argCount);
        vm.stackTop--;

        bool ok;
        if (!haveValue) {
          ok = invokeMethod(receiver, name, argCount);
        } else if (IS_CLOSURE(found) && wantsReceiver(AS_CLOSURE(found))) {
          /* callMethod reads the receiver out of the callee slot, so that slot
           * is left exactly as it is. */
          ok = callMethod(AS_CLOSURE(found), argCount);
        } else {
          vm.stackTop[-argCount - 1] = found;
          ok = callValue(found, argCount);
        }
        if (name != NULL) csPopTempRoot();
        if (!ok) return CS_RUNTIME_ERROR;

        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_DYNAMIC_IMPORT) {
        /* `import(specifier)` — a module named at run time.
         *
         * Reading and compiling a file is synchronous here, so the promise is
         * settled before it is handed back. That is a difference from
         * JavaScript only in *when* the work happens, not in what a program
         * can observe: the result is still a promise, so it is still reached
         * through `await` or `.then`, and the microtask that delivers it runs
         * in the same turn it would have anyway. */
        Value specifier = peekStack(0);
        if (!IS_STRING(specifier)) {
          csVMRuntimeError("import() expects a string, got %s",
                           csValueTypeName(specifier));
          return CS_RUNTIME_ERROR;
        }

        ObjPromise *promise = csPromiseNew();
        csVMPop();
        csVMPush(OBJ_VAL(promise));

        Value loaded;
        bool fatal = false;
        if (importAtRuntime(frame->closure->function->module, AS_STRING(specifier),
                            &loaded, &fatal)) {
          csPromiseFulfill(promise, loaded);
        } else if (fatal) {
          return CS_RUNTIME_ERROR;
        } else {
          /* A module that cannot be found or read rejects rather than stopping
           * the program, which is the whole point of asking for one at run
           * time. */
          csPromiseReject(promise, loaded);
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_NEW_TARGET)
        csVMPush(frame->newTarget);
        VM_NEXT();

      VM_CASE(OP_SET_PROTOTYPE) {
        /* `{ __proto__: base }`. The object being built is beneath the value. */
        Value value = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(1));

        if (IS_NULL(value) || IS_UNDEFINED(value)) {
          object->prototype = NULL;
        } else if (!IS_OBJECT(value)) {
          csVMRuntimeError("a prototype must be an object or null, got %s",
                           csValueTypeName(value));
          return CS_RUNTIME_ERROR;
        } else if (!csObjectSetPrototype(object, AS_OBJECT(value))) {
          csVMRuntimeError("that prototype is already in this object\'s chain, "
                           "which would make every lookup on it loop");
          return CS_RUNTIME_ERROR;
        }

        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_SET) {
        Value value = peekStack(0);
        Value keyValue = peekStack(1);
        ObjObject *object = AS_OBJECT(peekStack(2));

        if (IS_SYMBOL(keyValue)) {
          csObjectPutPrivate(object, AS_SYMBOL(keyValue)->key, value);
          vm.stackTop -= 2;
          VM_NEXT();
        }

        ObjString *key = objectKeyFor(keyValue);
        if (key == NULL) {
          csVMRuntimeError("out of memory building an object key");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)key);
        csObjectPut(object, key, value);
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_ACCESSOR) {
        uint8_t isGetter = READ_BYTE();
        Value closure = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(2));

        ObjString *name = objectKeyFor(peekStack(1));
        if (name == NULL) {
          csVMRuntimeError("out of memory building an accessor name");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)name);

        /* One class per object that needs one, made on demand. The property
         * paths already consult `klass` for accessors, so nothing else has to
         * learn about this. */
        if (object->klass == NULL) {
          object->klass = csClassNew(object->name);
          object->klass->isAccessorHolder = true;
        }
        csTableSet(isGetter ? &object->klass->getters : &object->klass->setters,
                   name, closure);

        /* The name also takes a slot, holding the stand-in, so that it is
         * enumerated in the order it was written. Shape mode has to go with
         * it: the read fast path would hand the stand-in back as a value. */
        csObjectLeaveShapeMode(object);
        csObjectPut(object, name, OBJ_VAL(vm.accessorMarker));
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_MERGE) {
        Value source = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(1));

        if (IS_OBJECT(source)) {
          ObjObject *from = AS_OBJECT(source);
          /* By index rather than by iterator, because csObjectPut on the
           * target cannot change the source. */
          for (int i = 0; i < csObjectCount(from); i++) {
            ObjString *key = csObjectKeyAt(from, i);
            if (!csObjectIsEnumerable(from, key)) continue;
            Value value;
            if (!csVMReadOwnProperty(from, key, &value)) return CS_RUNTIME_ERROR;
            csObjectPut(object, key, value);
          }
        } else if (IS_ARRAY(source)) {
          /* `{ ...[a, b] }` is `{ 0: a, 1: b }` — the indices are the keys. */
          ObjArray *array = AS_ARRAY(source);
          for (int i = 0; i < array->elements.count; i++) {
            char digits[16];
            int length = snprintf(digits, sizeof digits, "%d", i);
            ObjString *key = csStringCopy(digits, length);
            csPushTempRoot((Obj *)key);
            csObjectPut(object, key, array->elements.values[i]);
            csPopTempRoot();
          }
        } else if (!IS_NULL(source) && !IS_UNDEFINED(source)) {
          /* Spreading null or undefined contributes nothing, as in
           * JavaScript; anything else has no keys to contribute either. */
          csVMRuntimeError("cannot spread %s into an object",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_REST) {
        uint8_t taken = READ_BYTE();
        Value *names = vm.stackTop - taken;
        Value source = names[-1];

        if (!IS_OBJECT(source)) {
          csVMRuntimeError("cannot destructure %s as an object",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        ObjObject *rest = csObjectNew("Object");
        csPushTempRoot((Obj *)rest);

        ObjObject *from = AS_OBJECT(source);
        for (int i = 0; i < csObjectCount(from); i++) {
          ObjString *key = csObjectKeyAt(from, i);
          if (!csObjectIsEnumerable(from, key)) continue;

          bool named = false;
          for (int n = 0; n < taken && !named; n++) {
            named = IS_STRING(names[n]) && AS_STRING(names[n]) == key;
          }
          if (named) continue;

          Value value;
          if (!csVMReadOwnProperty(from, key, &value)) return CS_RUNTIME_ERROR;
          csObjectPut(rest, key, value);
        }

        csPopTempRoot();
        /* Replaces the source and the names, the shape OP_ARRAY_REST leaves. */
        vm.stackTop = names - 1;
        csVMPush(OBJ_VAL(rest));
        VM_NEXT();
      }

      VM_CASE(OP_TEMPLATE_STRINGS) {
        Value raw = peekStack(0);
        Value cooked = peekStack(1);
        csArrayPutExtra(AS_ARRAY(cooked), "raw", 3, raw);
        csVMPop();
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

      VM_CASE(OP_CLASS_MEMBER) {
        uint8_t kind = READ_BYTE();
        Value value = peekStack(0);
        ObjClass *klass = AS_CLASS(peekStack(2));

        ObjString *name = objectKeyFor(peekStack(1));
        if (name == NULL) {
          csVMRuntimeError("out of memory building a member name");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)name);

        Table *into = kind == 0   ? &klass->methods
                      : kind == 1 ? &klass->statics
                      : kind == 2 ? &klass->getters
                      : kind == 3 ? &klass->setters
                      : kind == 5 ? &klass->staticGetters
                      : kind == 6 ? &klass->staticSetters
                                  : &klass->statics;
        csTableSet(into, name, value);
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_GETTER)
      VM_CASE(OP_SETTER)
      VM_CASE(OP_STATIC_GETTER)
      VM_CASE(OP_STATIC_SETTER) {
        ObjString *name = READ_STRING();
        ObjClass *klass = AS_CLASS(peekStack(1));
        Table *into = instruction == OP_GETTER          ? &klass->getters
                      : instruction == OP_SETTER        ? &klass->setters
                      : instruction == OP_STATIC_GETTER ? &klass->staticGetters
                                                        : &klass->staticSetters;
        csTableSet(into, name, peekStack(0));
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

        /* A constructor function: `function Point(x) { this.x = x }`. What
         * makes it one is the `new`, not anything about how it was written —
         * which is JavaScript's rule exactly. The object being built inherits
         * from the function's `prototype`, so behaviour written there is
         * shared by everything the function constructs. */
        if (IS_CLOSURE(target)) {
          ObjClosure *constructor = AS_CLOSURE(target);
          if (constructor->function->isGenerator || constructor->function->isAsync) {
            csVMRuntimeError("'new' needs a class or an ordinary function, and "
                             "%s is neither",
                             constructor->function->isGenerator ? "a generator"
                                                                : "an async function");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }

          ObjObject *built = csObjectNew(constructor->function->name != NULL
                                             ? constructor->function->name->chars
                                             : "Object");
          csPushTempRoot((Obj *)built);
          built->builtByConstructor = constructor->function->name != NULL;
          built->prototype = csClosurePrototype(constructor);
          /* Where the class path puts the instance: slot 0, which the body
           * reads as `this`. */
          vm.stackTop[-argCount - 1] = OBJ_VAL(built);

          /* Run to completion in a loop of its own, because what the call
           * answers with decides the result: JavaScript lets a constructor
           * return a different object, and discards anything that is not one.
           * That choice cannot be made by leaving the frame to unwind. */
          Value returned;
          vm.pendingNewTarget = target;
          bool ok = csVMCallCallbackWithReceiver(OBJ_VAL(constructor), argCount,
                                                 &returned);
          vm.pendingNewTarget = UNDEFINED_VAL;
          csPopTempRoot();
          if (!ok) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          csVMPush(IS_OBJECT(returned) ? returned : OBJ_VAL(built));
          VM_NEXT();
        }

        if (!IS_CLASS(target)) {
          csVMRuntimeError("'new' needs a class or a function, got %s",
                           csValueTypeName(target));
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
          vm.pendingNewTarget = target;
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
        vm.pendingNewTarget = target;
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

      VM_CASE(OP_IN) {
        Value target = peekStack(0);
        Value key = peekStack(1);
        bool result;

        if (IS_OBJECT(target) && IS_SYMBOL(key)) {
          Value ignored;
          bool has = csObjectGetPrivate(AS_OBJECT(target), AS_SYMBOL(key)->key,
                                        &ignored);
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(has));
          VM_NEXT();
        }
        if (IS_OBJECT(target)) {
          ObjString *name = objectKeyFor(key);
          if (name == NULL) {
            csVMRuntimeError("out of memory converting a key for 'in'");
            return CS_RUNTIME_ERROR;
          }
          csPushTempRoot((Obj *)name);
          Value ignored;
          ObjObject *object = AS_OBJECT(target);
          result = csObjectGetInherited(object, name, &ignored) ||
                   (object->klass != NULL &&
                    csClassFindMethod(object->klass, name) != NULL);
          csPopTempRoot();
        } else if (IS_ARRAY(target)) {
          /* An array's own keys are its indices, so `2 in [a, b, c]` is true
           * and `3` is not — the same question `in` asks of an object. */
          ObjArray *array = AS_ARRAY(target);
          result = IS_NUMBER(key) && AS_NUMBER(key) >= 0 &&
                   AS_NUMBER(key) < (double)array->elements.count &&
                   AS_NUMBER(key) == (double)(long long)AS_NUMBER(key);
        } else {
          csVMRuntimeError("the right side of 'in' must be an object or an "
                           "array, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(result));
        VM_NEXT();
      }

      VM_CASE(OP_INSTANCEOF) {
        Value target = peekStack(0);
        Value value = peekStack(1);

        bool result;
        if (IS_CLOSURE(target)) {
          /* For a constructor function the question is the one JavaScript
           * asks: is the function's `prototype` anywhere in the value's chain?
           * A function nothing has been constructed from has no prototype
           * object yet, and nothing can be an instance of it. */
          ObjObject *prototype = AS_CLOSURE(target)->prototype;
          result = false;
          if (prototype != NULL && IS_OBJECT(value)) {
            for (ObjObject *at = AS_OBJECT(value)->prototype; at != NULL;
                 at = at->prototype) {
              if (at != prototype) continue;
              result = true;
              break;
            }
          }
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(result));
          VM_NEXT();
        }

        if (!IS_CLASS(target)) {
          csVMRuntimeError("the right side of 'instanceof' must be a class or a "
                           "function, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        result = IS_OBJECT(value) && AS_OBJECT(value)->klass != NULL &&
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
        Value namespaceValue;
        if (!buildNamespace(AS_MODULE(peekStack(0)), &namespaceValue)) {
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        csVMPush(namespaceValue);
        VM_NEXT();
      }

      VM_CASE(OP_YIELD) {
        ObjFiber *fiber = vm.currentFiber;
        if (fiber == NULL || fiber->generator == NULL) {
          csVMRuntimeError("'yield' outside a generator");
          return CS_RUNTIME_ERROR;
        }

        fiber->generator->yielded = csVMPop();
        /* The frame's ip is already past this instruction, so resuming lands
         * on the next one — and what `next(x)` pushes arrives exactly where
         * the yielded value was, which is what makes `const x = yield v` a
         * two-way exchange. */
        vm.fiberSuspended = true;
        vm.fiberYielded = true;
        return CS_OK;
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

      VM_CASE(OP_SUBTRACT) BINARY_ARITHMETIC_OP(-, '-'); VM_NEXT();
      VM_CASE(OP_MULTIPLY) BINARY_ARITHMETIC_OP(*, '*'); VM_NEXT();
      VM_CASE(OP_DIVIDE)   BINARY_ARITHMETIC_OP(/, '/'); VM_NEXT();

      VM_CASE(OP_MODULO) {
        if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {
          if (!bigintArithmetic('%', "%")) return CS_RUNTIME_ERROR;
          VM_NEXT();
        }
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
        if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {
          if (!bigintArithmetic('p', "**")) return CS_RUNTIME_ERROR;
          VM_NEXT();
        }
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
        if (IS_BIGINT(peekStack(0))) {
          BigInt flipped;
          csBigInit(&flipped);
          if (!csBigNegate(&flipped, &AS_BIGINT(peekStack(0))->value)) {
            csBigFree(&flipped);
            csVMRuntimeError("out of memory negating a BigInt");
            return CS_RUNTIME_ERROR;
          }
          ObjBigInt *negated = csBigIntNew(flipped);
          csVMPop();
          csVMPush(OBJ_VAL(negated));
          VM_NEXT();
        }
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

      VM_CASE(OP_GREATER)       BINARY_COMPARE_OP(>); VM_NEXT();
      VM_CASE(OP_GREATER_EQUAL) BINARY_COMPARE_OP(>=); VM_NEXT();
      VM_CASE(OP_LESS)          BINARY_COMPARE_OP(<); VM_NEXT();
      VM_CASE(OP_LESS_EQUAL)    BINARY_COMPARE_OP(<=); VM_NEXT();

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
      /* Nullish, not falsy: `0 ?? 1` is 0 and `"" ?? "x"` is "". Getting this
       * wrong would make `??` an alias for `||`, which is the whole reason the
       * operator was added to JavaScript. */
      VM_CASE(OP_JUMP_IF_NULLISH) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        if (IS_NULL(top) || IS_UNDEFINED(top)) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_NO_METHOD) {
        ObjString *name = READ_STRING();
        uint16_t offset = READ_SHORT();
        if (!receiverHasMethod(peekStack(0), name)) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_NOT_NULLISH) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        if (!IS_NULL(top) && !IS_UNDEFINED(top)) frame->ip += offset;
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
    if (IS_BIGINT(a) || IS_BIGINT(b)) {                                        \
      int order = 0;                                                           \
      OrderResult ordering = comparingExactly(a, b, &order);                   \
      if (ordering == ORDER_INVALID) return CS_RUNTIME_ERROR;                  \
      vm.stackTop -= 2;                                                        \
      if (!(ordering == ORDER_KNOWN && (order op 0))) frame->ip += offset;     \
      break;                                                                   \
    }                                                                          \
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

#ifdef CS_DEBUG_JIT
        /* And having counted it, this is where the loop is handed over.
         *
         * Compiling at the call site cannot help a loop: the function a
         * back-edge makes hot is already running, and for `heavy` — one call
         * around twenty million iterations — the compiled code was entered
         * exactly zero times. The guard is one load and a compare, paid only
         * on a back-edge, and it fails for every function that never
         * compiled. */
        if (frame->closure->function->jitState == JIT_COMPILED) {
          Value produced;
          int resumeAt = -1;
          int resumeHeight = 0;
          if (csJitOsr(frame->closure->function,
                       (int)(frame->ip - frame->closure->function->chunk.code),
                       frame->slots, &produced, &resumeAt, &resumeHeight)) {
            /* Handed back part-way: the compiled code took the loop and left
             * whatever follows it alone. The frame is already in the shape the
             * interpreter expects — the slots are the same slots — so only the
             * two things the compiled code does not keep have to be put back:
             * where to resume, and how deep the operand stack is. */
            if (resumeAt >= 0) {
              frame->ip = frame->closure->function->chunk.code + resumeAt;
              vm.stackTop = frame->slots + resumeHeight;
              VM_NEXT();
            }

            /* The compiled code ran the function to completion, so what is
             * left is the frame teardown OP_RETURN does — kept in step with
             * it deliberately rather than shared, because the two differ in
             * where the result comes from and in nothing else. */
            while (vm.handlerCount > 0 &&
                   vm.handlers[vm.handlerCount - 1].frameCount >= vm.frameCount) {
              vm.handlerCount--;
            }
            closeUpvalues(frame->slots);
            vm.frameCount--;

            vm.stackTop = frame->slots;
            csVMPush(produced);
            if (vm.frameCount == baseFrame) return CS_OK;
            frame = &vm.frames[vm.frameCount - 1];
          }
        }
#endif
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
#undef BINARY_ARITHMETIC_OP
#undef BINARY_COMPARE_OP
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

/* A top level that awaits.
 *
 * `await` suspends a fiber, and only an async body has one — so a file with a
 * top-level await runs as an async call and the loop is driven here until it
 * settles. Driving it *here* rather than leaving it to the end is what keeps
 * imports in order: a module that awaits has finished before the module that
 * imported it starts, which is the guarantee the whole loader is built on. */
static InterpretResult runBodyAsync(ObjClosure *closure) {
  csVMPush(OBJ_VAL(closure));
  if (!callAsyncFunction(closure, 0)) {
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  ObjPromise *promise = AS_PROMISE(csVMPop());
  csPushTempRoot((Obj *)promise);
  /* Whatever it settles as is reported below, so the watchdog must not report
   * it first and count it twice. */
  promise->handled = true;

  InterpretResult result = csVMRunEventLoop();
  csPopTempRoot();
  if (result != CS_OK) {
    resetStack();
    return result;
  }

  if (promise->state == PROMISE_REJECTED) {
    fflush(stdout);
    size_t length = 0;
    char *text = csValueInspect(promise->value, &length);
    fprintf(stderr, "cscript: uncaught error at the top level: %s\n",
            text != NULL ? text : "<unprintable>");
    free(text);
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  if (promise->state == PROMISE_PENDING) {
    /* Nothing is left to run and it never settled, so nothing ever will. */
    csVMRuntimeError("a top-level 'await' is waiting for something that will "
                     "never happen");
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  resetStack();
  return CS_OK;
}

InterpretResult csVMRunBody(ObjFunction *body) {
  /* A top level is itself a function, so running it is just a call. Pushing
   * the closure first keeps it reachable while callClosure allocates nothing
   * but still leaves it rooted through the frame. */
  csPushTempRoot((Obj *)body);
  ObjClosure *closure = csClosureNew(body);
  csPopTempRoot();

  if (body->isAsync) return runBodyAsync(closure);

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
    /* The receiver is in place, so re-entering must not blank it. A generator
     * or an async body pushes no frame here, so those take the ordinary path,
     * which puts the receiver where their fiber's slot 0 will be. */
    ObjClosure *method = bound->method->type == OBJ_CLOSURE
                             ? (ObjClosure *)bound->method
                             : NULL;
    if (method != NULL && !method->function->isGenerator &&
        !method->function->isAsync) {
      return csVMCallCallbackWithReceiver(OBJ_VAL(method), argCount, result);
    }
    return csVMCallCallback(OBJ_VAL(bound->method), argCount, result);
  }

  if (!IS_CLOSURE(callee)) {
    csVMRuntimeError("%s is not a function", csValueTypeName(callee));
    return false;
  }

  /* Same again: a generator call pushes no frame either, because none of the
   * body runs until something pulls. */
  if (AS_CLOSURE(callee)->function->isGenerator) {
    if (!callGeneratorFunction(AS_CLOSURE(callee), argCount)) return false;
    *result = csVMPop();
    return true;
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

/* The same, for a call whose slot 0 already holds the receiver. `new` on a
 * plain function needs it: the object being built is there, and blanking it
 * the way an ordinary call does would take `this` away from the constructor. */
bool csVMCallCallbackWithReceiver(Value callee, int argCount, Value *result) {
  int baseFrame = vm.frameCount;
  if (!csVMCallClosureWith(AS_CLOSURE(callee), argCount, true)) return false;
  if (run(baseFrame) != CS_OK) return false;
  *result = csVMPop();
  return true;
}
