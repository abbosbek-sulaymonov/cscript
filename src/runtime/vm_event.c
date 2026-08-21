/* vm_event.c — the microtask queue, timers, and the loop that drains them.
 *
 * Settling a promise queues its reactions rather than calling them, which is\n * the one rule that makes ordering predictable — and the reason a handler never\n * runs in the middle of the code that settled the promise. Everything here\n * runs after the program's own code has finished, so none of it is hot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"
#include "runtime/vm_internal.h"


void csVMQueueMicrotask(Value callback, Value argument, ObjPromise *result,
                        bool isRejection) {
  /* Compact before growing: the queue is drained to empty between macrotasks,
   * so the head is almost always reclaimable. */
  if (vm.microtaskHead > 0 && vm.microtaskHead == vm.microtaskCount) {
    vm.microtaskHead = 0;
    vm.microtaskCount = 0;
  }

  if (vm.microtaskCapacity < vm.microtaskCount + 1) {
    int oldCapacity = vm.microtaskCapacity;
    vm.microtaskCapacity = CS_GROW_CAPACITY(oldCapacity);
    vm.microtasks =
        CS_GROW_ARRAY(Microtask, vm.microtasks, oldCapacity, vm.microtaskCapacity);
  }

  Microtask *task = &vm.microtasks[vm.microtaskCount++];
  task->callback = callback;
  task->argument = argument;
  task->result = result;
  task->isRejection = isRejection;
  task->combineState = NULL;
  task->combineIndex = 0;
  task->fiber = NULL;
  task->isFinally = false;
  task->extraHops = 0;
}

Microtask *csVMLastMicrotask(void) { return &vm.microtasks[vm.microtaskCount - 1]; }

void csVMQueueCombine(ObjArray *state, int index, Value argument, bool isRejection) {
  csVMQueueMicrotask(UNDEFINED_VAL, argument, NULL, isRejection);
  Microtask *task = &vm.microtasks[vm.microtaskCount - 1];
  task->combineState = state;
  task->combineIndex = index;
}

/* `{ status, value }` or `{ status, reason }`, which is the only thing
 * allSettled reports and the reason it never rejects. */
static Value settlementRecord(Value outcome, bool isRejection) {
  ObjObject *record = csObjectNew("Object");
  csPushTempRoot((Obj *)record);
  if (IS_OBJ(outcome)) csPushTempRoot(AS_OBJ(outcome));

  ObjString *status = csStringCopy(isRejection ? "rejected" : "fulfilled",
                                   isRejection ? 8 : 9);
  csPushTempRoot((Obj *)status);
  csObjectSetProperty(record, "status", OBJ_VAL(status));
  csPopTempRoot();

  csObjectSetProperty(record, isRejection ? "reason" : "value", outcome);

  if (IS_OBJ(outcome)) csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(record);
}

/* The four combinators, settled from the inside. The state is
 * [results, remaining, target, mode]; a negative index means the first
 * settlement wins, which is race. */
void runCombine(const Microtask *task) {
  ObjArray *state = task->combineState;
  ObjPromise *target = AS_PROMISE(state->elements.values[2]);
  CombineMode mode = (CombineMode)AS_NUMBER(state->elements.values[3]);

  /* A combinator that has already decided ignores the rest, which is what
   * lets `race` and `any` leave their losers running harmlessly. */
  if (target->state != PROMISE_PENDING) return;

  switch (mode) {
    case COMBINE_RACE:
      if (task->isRejection) {
        csPromiseReject(target, task->argument);
      } else {
        csPromiseFulfill(target, task->argument);
      }
      return;

    case COMBINE_ALL:
      /* The first rejection is the answer; the rest no longer matter. */
      if (task->isRejection) {
        csPromiseReject(target, task->argument);
        return;
      }
      break;

    case COMBINE_ANY:
      /* The mirror image: the first fulfilment wins, and rejections are only
       * collected in case every one of them rejects. */
      if (!task->isRejection) {
        csPromiseFulfill(target, task->argument);
        return;
      }
      break;

    case COMBINE_ALL_SETTLED:
      break;
  }

  ObjArray *results = AS_ARRAY(state->elements.values[0]);
  results->elements.values[task->combineIndex] =
      mode == COMBINE_ALL_SETTLED
          ? settlementRecord(task->argument, task->isRejection)
          : task->argument;

  double remaining = AS_NUMBER(state->elements.values[1]) - 1;
  state->elements.values[1] = NUMBER_VAL(remaining);
  if (remaining != 0) return;

  if (mode == COMBINE_ANY) {
    csPromiseReject(target, csAggregateError(results));
  } else {
    csPromiseFulfill(target, state->elements.values[0]);
  }
}

void csVMNoteRejection(ObjPromise *promise) {
  if (vm.rejectedCapacity < vm.rejectedCount + 1) {
    int oldCapacity = vm.rejectedCapacity;
    vm.rejectedCapacity = CS_GROW_CAPACITY(oldCapacity);
    vm.rejected =
        CS_GROW_ARRAY(ObjPromise *, vm.rejected, oldCapacity, vm.rejectedCapacity);
  }
  vm.rejected[vm.rejectedCount++] = promise;
}

/* Milliseconds on a clock that only moves forward, so a timer cannot be
 * delayed or fired early by the wall clock being adjusted. */
double nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* Runs one queued reaction. A microtask always ends by settling its result
 * promise — with what the handler returned, with what it threw, or with the
 * value that arrived when there was no handler to ask. */
InterpretResult runMicrotask(const Microtask *task) {
  if (task->fiber != NULL) {
    /* An `await` that is ready to continue. */
    csVMResumeFiber(task->fiber, task->argument, task->isRejection);
    return CS_OK;
  }

  if (task->combineState != NULL) {
    runCombine(task);
    return CS_OK;
  }

  Value callback = task->callback;
  Value argument = task->argument;
  ObjPromise *result = task->result;
  bool isRejection = task->isRejection;

  /* No handler for this outcome: it passes straight through, which is what
   * makes `.then(f)` forward a rejection and `.catch(g)` forward a value. */
  if (IS_UNDEFINED(callback) || IS_NULL(callback)) {
    if (task->extraHops > 0) {
      csVMQueueMicrotask(UNDEFINED_VAL, argument, result, isRejection);
      csVMLastMicrotask()->extraHops = task->extraHops - 1;
      return CS_OK;
    }
    if (result != NULL) {
      if (isRejection) {
        csPromiseReject(result, argument);
      } else {
        csPromiseFulfill(result, argument);
      }
    }
    return CS_OK;
  }

  if (result != NULL) csPushTempRoot((Obj *)result);

  /* Adapted rather than called with exactly one argument: a handler that
   * ignores the value — `queueMicrotask(() => ...)` — declares none. */
  Value handlerArgs[1] = {argument};
  Value returned;
  /* A throw that escapes the handler belongs to the promise this feeds, not to
   * the program: it becomes that promise's rejection. */
  bool wasDeferring = vm.deferUncaught;
  vm.deferUncaught = true;
  bool ok = csVMCallAdapted(callback, handlerArgs, 1, &returned);
  vm.deferUncaught = wasDeferring;
  if (result != NULL) csPopTempRoot();

  if (!ok) {
    if (vm.hasPendingException) {
      Value thrown = vm.pendingException;
      vm.hasPendingException = false;
      resetStack();
      if (result != NULL) {
        csPromiseReject(result, thrown);
        return CS_OK;
      }
      csVMRuntimeError("uncaught exception in a microtask");
      return CS_RUNTIME_ERROR;
    }
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  if (result != NULL) {
    /* Settling queues reactions, which can allocate — and `returned` is a bare
     * C local by now. */
    if (IS_OBJ(returned)) csPushTempRoot(AS_OBJ(returned));
    if (task->isFinally) {
      /* `.finally` ran for its effect. What it returned is discarded and the
       * outcome it was told about carries on unchanged — one tick later,
       * because the specification composes `.finally` out of a `.then` that
       * waits on the handler's result before passing the outcome along, and a
       * program can observe the difference in the order its handlers run. */
      csVMQueueMicrotask(UNDEFINED_VAL, argument, result, isRejection);
      csVMLastMicrotask()->extraHops = 1;
    } else {
      csPromiseFulfill(result, returned);
    }
    if (IS_OBJ(returned)) csPopTempRoot();
  }
  return CS_OK;
}

InterpretResult drainMicrotasks(void) {
  while (vm.microtaskHead < vm.microtaskCount) {
    /* The head stays put until the task is done. The collector marks the queue
     * from the head onward, and everything a task holds — its handler, the
     * value it carries, the promise it settles — is reachable from nowhere
     * else while it runs. Advancing first would unmark all three. */
    Microtask task = vm.microtasks[vm.microtaskHead];
    InterpretResult result = runMicrotask(&task);
    vm.microtaskHead++;
    if (result != CS_OK) return result;
  }
  vm.microtaskHead = 0;
  vm.microtaskCount = 0;
  return CS_OK;
}

/* Reports a rejection nothing ever listened to. Node treats this as fatal and
 * so does CScript: a promise that failed with no one watching is a bug, and
 * the alternative is a program that silently does half its work. */
InterpretResult reportUnhandledRejections(void) {
  for (int i = 0; i < vm.rejectedCount; i++) {
    ObjPromise *promise = vm.rejected[i];
    if (promise->handled) continue;

    fflush(stdout);
    size_t length = 0;
    char *text = csValueInspect(promise->value, &length);
    fprintf(stderr, "cscript: unhandled promise rejection: %s\n",
            text != NULL ? text : "<unprintable>");
    free(text);
    vm.rejectedCount = 0;
    return CS_RUNTIME_ERROR;
  }
  vm.rejectedCount = 0;
  return CS_OK;
}

InterpretResult csVMRunEventLoop(void) {
  for (;;) {
    InterpretResult result = drainMicrotasks();
    if (result != CS_OK) return result;

    /* Checked at every turn of the loop rather than once at the end: a
     * rejection that nothing was listening to when the queue ran dry is
     * already a bug, and waiting would let a later handler hide it. */
    result = reportUnhandledRejections();
    if (result != CS_OK) return result;

    /* Timers are kept sorted, so the next one to run is the first one left. */
    if (vm.timerCount == 0) break;

    Timer next = vm.timers[0];
    if (next.cancelled) {
      memmove(&vm.timers[0], &vm.timers[1], sizeof(Timer) * (size_t)(vm.timerCount - 1));
      vm.timerCount--;
      continue;
    }

    double wait = next.dueMs - nowMs();
    if (wait > 0) {
      struct timespec sleepFor;
      sleepFor.tv_sec = (time_t)(wait / 1000.0);
      sleepFor.tv_nsec = (long)((wait - (double)sleepFor.tv_sec * 1000.0) * 1000000.0);
      nanosleep(&sleepFor, NULL);
    }

    memmove(&vm.timers[0], &vm.timers[1], sizeof(Timer) * (size_t)(vm.timerCount - 1));
    vm.timerCount--;

    /* Off the queue, so the collector no longer reaches it through the timer
     * list — the same trap the microtask loop avoids by not advancing. */
    if (IS_OBJ(next.callback)) csPushTempRoot(AS_OBJ(next.callback));
    vm.firingTimerId = next.id;
    vm.firingCancelled = false;
    Value ignored;
    if (!csVMCallAdapted(next.callback, NULL, 0, &ignored)) {
      if (vm.hasPendingException) {
        Value thrown = vm.pendingException;
        vm.hasPendingException = false;
        resetStack();
        csVMPush(thrown);
        csVMRuntimeError("uncaught exception in a timer callback");
      }
      resetStack();
      if (IS_OBJ(next.callback)) csPopTempRoot();
      return CS_RUNTIME_ERROR;
    }
    if (IS_OBJ(next.callback)) csPopTempRoot();
    resetStack();

    /* An interval goes back on the queue only now, so a callback that took
     * longer than the interval cannot pile up behind itself. `clearInterval`
     * during the callback wins, because the handle is gone by then. */
    bool stopped = vm.firingCancelled;
    vm.firingTimerId = -1;
    vm.firingCancelled = false;

    if (next.repeatMs > 0 && !stopped && vm.timerCount < CS_TIMERS_MAX) {
      Timer again = next;
      again.dueMs = nowMs() + next.repeatMs;
      again.sequence = vm.timerSequence++;

      int at = vm.timerCount;
      while (at > 0 && (vm.timers[at - 1].dueMs > again.dueMs ||
                        (vm.timers[at - 1].dueMs == again.dueMs &&
                         vm.timers[at - 1].sequence > again.sequence))) {
        vm.timers[at] = vm.timers[at - 1];
        at--;
      }
      vm.timers[at] = again;
      vm.timerCount++;
    }
  }

  return reportUnhandledRejections();
}
