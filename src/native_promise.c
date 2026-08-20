/* native_promise.c — promises, the microtask-driven part of the event loop,
 * and timers.
 *
 * A promise is a value that stands for a result that has not arrived. What
 * makes it different from a callback is that settling it *queues* the waiting
 * reactions rather than running them, so a handler never runs in the middle of
 * the code that settled the promise. That single rule is what makes ordering
 * predictable, and it is why `.then` is asynchronous even for an already
 * settled promise.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* Milliseconds on a clock that only moves forward. */
static double nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static bool notAPromise(Value receiver, const char *method) {
  if (IS_PROMISE(receiver)) return false;
  csVMRuntimeError("'%s' can only be called on a promise", method);
  return true;
}

/* `.then(onFulfilled, onRejected)` — the one primitive; catch and finally are
 * both written in terms of it. */
static bool promiseThen(Value receiver, int argCount, Value *args, Value *result) {
  if (notAPromise(receiver, "then")) return false;

  Value onFulfilled = argCount > 0 ? args[0] : UNDEFINED_VAL;
  Value onRejected = argCount > 1 ? args[1] : UNDEFINED_VAL;

  /* Registering can grow the reaction array, and the new promise is reachable
   * from nothing but this local until it is returned. */
  ObjPromise *derived = csPromiseNew();
  csPushTempRoot((Obj *)derived);
  csPromiseAddReaction(AS_PROMISE(receiver), onFulfilled, onRejected, derived);
  csPopTempRoot();
  *result = OBJ_VAL(derived);
  return true;
}

static bool promiseCatch(Value receiver, int argCount, Value *args, Value *result) {
  if (notAPromise(receiver, "catch")) return false;

  ObjPromise *derived = csPromiseNew();
  csPushTempRoot((Obj *)derived);
  csPromiseAddReaction(AS_PROMISE(receiver), UNDEFINED_VAL,
                       argCount > 0 ? args[0] : UNDEFINED_VAL, derived);
  csPopTempRoot();
  *result = OBJ_VAL(derived);
  return true;
}

/* `.finally(f)` runs `f` whichever way the promise went, then passes the
 * original outcome along untouched — so it observes without participating.
 * Registering the same handler on both sides gets the "either way" part; the
 * isFinally flag is what stops the handler's return value from replacing the
 * value, and what keeps a rejection a rejection. */
static bool promiseFinally(Value receiver, int argCount, Value *args, Value *result) {
  if (notAPromise(receiver, "finally")) return false;
  Value handler = argCount > 0 ? args[0] : UNDEFINED_VAL;

  ObjPromise *source = AS_PROMISE(receiver);
  ObjPromise *derived = csPromiseNew();
  csPushTempRoot((Obj *)derived);
  csPromiseAddReaction(source, handler, handler, derived);
  if (source->state == PROMISE_PENDING) {
    source->reactions[source->reactionCount - 1].isFinally = true;
  } else {
    csVMLastMicrotask()->isFinally = true;
  }
  csPopTempRoot();

  *result = OBJ_VAL(derived);
  return true;
}

static bool promiseResolve(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  Value value = argCount > 0 ? args[0] : UNDEFINED_VAL;
  if (IS_PROMISE(value)) {
    /* Already a promise: resolving it again would only add a layer. */
    *result = value;
    return true;
  }
  ObjPromise *promise = csPromiseNew();
  csPushTempRoot((Obj *)promise);
  csPromiseFulfill(promise, value);
  csPopTempRoot();
  *result = OBJ_VAL(promise);
  return true;
}

static bool promiseRejectStatic(Value receiver, int argCount, Value *args,
                                Value *result) {
  (void)receiver;
  ObjPromise *promise = csPromiseNew();
  csPushTempRoot((Obj *)promise);
  csPromiseReject(promise, argCount > 0 ? args[0] : UNDEFINED_VAL);
  csPopTempRoot();
  *result = OBJ_VAL(promise);
  return true;
}


/* The two functions `new Promise(executor)` hands its callback. Each is a
 * native bound to the promise it settles — see ObjBoundMethod for why binding
 * a native is all a built-in needs to carry state. */
static bool resolveFunction(Value receiver, int argCount, Value *args, Value *result) {
  csPromiseFulfill(AS_PROMISE(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL);
  *result = UNDEFINED_VAL;
  return true;
}

static bool rejectFunction(Value receiver, int argCount, Value *args, Value *result) {
  csPromiseReject(AS_PROMISE(receiver), argCount > 0 ? args[0] : UNDEFINED_VAL);
  *result = UNDEFINED_VAL;
  return true;
}

static ObjNative *resolveNative = NULL;
static ObjNative *rejectNative = NULL;

static bool promiseConstruct(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount != 1 || (!IS_CLOSURE(args[0]) && !IS_BOUND_METHOD(args[0]) &&
                        !IS_NATIVE(args[0]))) {
    csVMRuntimeError("Promise expects a function taking (resolve, reject)");
    return false;
  }
  Value executor = args[0];

  ObjPromise *promise = csPromiseNew();
  csPushTempRoot((Obj *)promise);

  ObjBoundMethod *resolve = csBoundMethodNew(OBJ_VAL(promise), (Obj *)resolveNative);
  csPushTempRoot((Obj *)resolve);
  ObjBoundMethod *reject = csBoundMethodNew(OBJ_VAL(promise), (Obj *)rejectNative);
  csPushTempRoot((Obj *)reject);

  /* The executor runs immediately and synchronously, which is what makes
   * `new Promise` usable for wrapping a callback API. csVMCallAdapted pushes
   * the callee and the arguments itself and cleans them up again. */
  Value callArgs[2] = {OBJ_VAL(resolve), OBJ_VAL(reject)};
  Value ignored;
  bool ok = csVMCallAdapted(executor, callArgs, 2, &ignored);

  csPopTempRoot();
  csPopTempRoot();
  csPopTempRoot();
  if (!ok) return false;

  *result = OBJ_VAL(promise);
  return true;
}

/* Builds the [results, remaining, target, mode] state the combinators share. */
static ObjArray *combineState(int count, ObjPromise *target, CombineMode mode) {
  ObjArray *results = csArrayNew();
  csPushTempRoot((Obj *)results);
  for (int i = 0; i < count; i++) csValueArrayWrite(&results->elements, UNDEFINED_VAL);

  ObjArray *state = csArrayNew();
  csPushTempRoot((Obj *)state);
  csValueArrayWrite(&state->elements, OBJ_VAL(results));
  csValueArrayWrite(&state->elements, NUMBER_VAL(count));
  csValueArrayWrite(&state->elements, OBJ_VAL(target));
  csValueArrayWrite(&state->elements, NUMBER_VAL((double)mode));
  csPopTempRoot();
  csPopTempRoot();
  return state;
}

static bool combinator(int argCount, Value *args, Value *result, CombineMode mode) {
  static const char *names[] = {"Promise.all", "Promise.race",
                                "Promise.allSettled", "Promise.any"};
  const char *name = names[mode];

  if (argCount != 1 || !IS_ARRAY(args[0])) {
    csVMRuntimeError("%s expects an array", name);
    return false;
  }
  ObjArray *inputs = AS_ARRAY(args[0]);
  int count = inputs->elements.count;

  ObjPromise *target = csPromiseNew();
  csPushTempRoot((Obj *)target);

  /* Nothing to wait for. `all` and `allSettled` are already done; `any` has
   * nothing that could ever succeed; and an empty `race` never settles, which
   * is what the specification says and is worth not "fixing". */
  if (count == 0) {
    if (mode == COMBINE_ALL || mode == COMBINE_ALL_SETTLED) {
      csPromiseFulfill(target, OBJ_VAL(csArrayNew()));
    } else if (mode == COMBINE_ANY) {
      ObjArray *none = csArrayNew();
      csPushTempRoot((Obj *)none);
      csPromiseReject(target, csAggregateError(none));
      csPopTempRoot();
    }
    csPopTempRoot();
    *result = OBJ_VAL(target);
    return true;
  }

  /* Race is the one that keeps no per-input slot: whichever settles first is
   * the answer, so there is nothing to collect. */
  bool byIndex = mode != COMBINE_RACE;
  ObjArray *state = combineState(byIndex ? count : 0, target, mode);
  csPushTempRoot((Obj *)state);

  for (int i = 0; i < count; i++) {
    Value element = inputs->elements.values[i];
    if (!IS_PROMISE(element)) {
      /* A plain value counts as already fulfilled, so mixing them is fine. */
      csVMQueueCombine(state, byIndex ? i : -1, element, false);
      continue;
    }
    ObjPromise *input = AS_PROMISE(element);
    input->handled = true;
    csPromiseAddReaction(input, UNDEFINED_VAL, UNDEFINED_VAL, NULL);
    /* csPromiseAddReaction queued a plain pass-through when the promise had
     * already settled; the combinator fields are attached here either way. */
    if (input->state == PROMISE_PENDING) {
      Reaction *reaction = &input->reactions[input->reactionCount - 1];
      reaction->combineState = state;
      reaction->combineIndex = byIndex ? i : -1;
    } else {
      Microtask *task = &vm.microtasks[vm.microtaskCount - 1];
      task->combineState = state;
      task->combineIndex = byIndex ? i : -1;
    }
  }

  csPopTempRoot();
  csPopTempRoot();
  *result = OBJ_VAL(target);
  return true;
}

static bool promiseAll(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return combinator(argCount, args, result, COMBINE_ALL);
}

static bool promiseRace(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return combinator(argCount, args, result, COMBINE_RACE);
}

static bool promiseAllSettled(Value receiver, int argCount, Value *args,
                              Value *result) {
  (void)receiver;
  return combinator(argCount, args, result, COMBINE_ALL_SETTLED);
}

static bool promiseAny(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return combinator(argCount, args, result, COMBINE_ANY);
}

/* Timers. Kept sorted by due time, ties broken by registration order, so the
 * output of a program with several timers is reproducible. */
static bool scheduleTimer(int argCount, Value *args, Value *result,
                          const char *name, bool repeating) {
  if (argCount < 1) {
    csVMRuntimeError("%s expects a function and an optional delay", name);
    return false;
  }
  double delay = argCount > 1 && IS_NUMBER(args[1]) ? AS_NUMBER(args[1]) : 0;
  if (delay < 0 || delay != delay) delay = 0;

  if (vm.timerCount == CS_TIMERS_MAX) {
    csVMRuntimeError("too many pending timers (limit %d)", CS_TIMERS_MAX);
    return false;
  }

  Timer timer;
  timer.dueMs = nowMs() + delay;
  timer.sequence = vm.timerSequence++;
  timer.id = vm.nextTimerId++;
  timer.callback = args[0];
  timer.cancelled = false;
  /* An interval with no delay would spin the loop with nothing between turns,
   * so it is treated as the shortest one a browser would give it. */
  timer.repeatMs = repeating ? (delay > 1 ? delay : 1) : 0;

  int at = vm.timerCount;
  while (at > 0 && (vm.timers[at - 1].dueMs > timer.dueMs ||
                    (vm.timers[at - 1].dueMs == timer.dueMs &&
                     vm.timers[at - 1].sequence > timer.sequence))) {
    vm.timers[at] = vm.timers[at - 1];
    at--;
  }
  vm.timers[at] = timer;
  vm.timerCount++;

  *result = NUMBER_VAL(timer.id);
  return true;
}

static bool setTimeoutNative(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  return scheduleTimer(argCount, args, result, "setTimeout", false);
}

static bool setIntervalNative(Value receiver, int argCount, Value *args,
                              Value *result) {
  (void)receiver;
  return scheduleTimer(argCount, args, result, "setInterval", true);
}

static bool clearTimeoutNative(Value receiver, int argCount, Value *args,
                               Value *result) {
  (void)receiver;
  *result = UNDEFINED_VAL;
  if (argCount < 1 || !IS_NUMBER(args[0])) return true;

  int id = (int)AS_NUMBER(args[0]);

  /* The interval that is running right now is not in the queue to be found,
   * so it is marked separately and the loop checks before re-arming it. */
  if (id == vm.firingTimerId) vm.firingCancelled = true;

  for (int i = 0; i < vm.timerCount; i++) {
    if (vm.timers[i].id != id) continue;
    memmove(&vm.timers[i], &vm.timers[i + 1],
            sizeof(Timer) * (size_t)(vm.timerCount - i - 1));
    vm.timerCount--;
    break;
  }
  return true;
}

static bool queueMicrotaskNative(Value receiver, int argCount, Value *args,
                                 Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("queueMicrotask expects a function");
    return false;
  }
  csVMQueueMicrotask(args[0], UNDEFINED_VAL, NULL, false);
  *result = UNDEFINED_VAL;
  return true;
}

static void defineMethod(Table *table, const char *name, NativeFn function, int arity) {
  ObjNative *native = csNativeNew(function, name, arity);
  csPushTempRoot((Obj *)native);
  ObjString *key = csStringCopy(name, (int)strlen(name));
  csPushTempRoot((Obj *)key);
  csTableSet(table, key, OBJ_VAL(native));
  csPopTempRoot();
  csPopTempRoot();
}

void csPromiseMethodsInstall(void) {
  defineMethod(&vm.promiseMethods, "then", promiseThen, -1);
  defineMethod(&vm.promiseMethods, "catch", promiseCatch, -1);
  defineMethod(&vm.promiseMethods, "finally", promiseFinally, -1);

  /* Kept alive by the globals that reference them via the bound methods they
   * are handed to; rooted here because they are also reached from C. */
  resolveNative = csNativeNew(resolveFunction, "resolve", -1);
  rejectNative = csNativeNew(rejectFunction, "reject", -1);
}

void csPromiseMarkRoots(void) {
  csMarkObject((Obj *)resolveNative);
  csMarkObject((Obj *)rejectNative);
}

ObjNative *csPromiseConstructor(void) {
  return csNativeNew(promiseConstruct, "Promise", -1);
}

void csPromiseInstallStatics(ObjObject *statics) {
  ObjNative *resolve = csNativeNew(promiseResolve, "resolve", -1);
  csPushTempRoot((Obj *)resolve);
  csObjectSetProperty(statics, "resolve", OBJ_VAL(resolve));
  csPopTempRoot();

  ObjNative *reject = csNativeNew(promiseRejectStatic, "reject", -1);
  csPushTempRoot((Obj *)reject);
  csObjectSetProperty(statics, "reject", OBJ_VAL(reject));
  csPopTempRoot();

  ObjNative *all = csNativeNew(promiseAll, "all", -1);
  csPushTempRoot((Obj *)all);
  csObjectSetProperty(statics, "all", OBJ_VAL(all));
  csPopTempRoot();

  ObjNative *allSettled = csNativeNew(promiseAllSettled, "allSettled", -1);
  csPushTempRoot((Obj *)allSettled);
  csObjectSetProperty(statics, "allSettled", OBJ_VAL(allSettled));
  csPopTempRoot();

  ObjNative *anyOf = csNativeNew(promiseAny, "any", -1);
  csPushTempRoot((Obj *)anyOf);
  csObjectSetProperty(statics, "any", OBJ_VAL(anyOf));
  csPopTempRoot();

  ObjNative *race = csNativeNew(promiseRace, "race", -1);
  csPushTempRoot((Obj *)race);
  csObjectSetProperty(statics, "race", OBJ_VAL(race));
  csPopTempRoot();
}

NativeFn csSetTimeoutFn(void) { return setTimeoutNative; }
NativeFn csSetIntervalFn(void) { return setIntervalNative; }
NativeFn csClearTimeoutFn(void) { return clearTimeoutNative; }
NativeFn csQueueMicrotaskFn(void) { return queueMicrotaskNative; }
