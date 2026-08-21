/* vm_fiber.c — suspendable calls.
 *
 * `await` stops a running function and starts it again later. Copying its\n * slots off the value stack would be cheaper, but upvalues hold raw pointers\n * *into* that stack — so a suspendable call gets a stack of its own and\n * nothing ever moves. The active fiber's state lives inline in the VM, which\n * keeps the interpreter loop unchanged; switching swaps it out and back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/vm.h"
#include "runtime/vm_internal.h"


/* Moves the active execution state into `fiber`, and `fiber`'s into the VM.
 * The VM keeps the running state inline so the interpreter loop never pays for
 * an indirection; this is the price of that, paid once per suspend or resume. */
void swapExecutionState(ObjFiber *fiber) {
  Value *stack = vm.stack;
  Value *stackTop = vm.stackTop;
  int stackCapacity = vm.stackCapacity;
  CallFrame *frames = vm.frames;
  int frameCount = vm.frameCount;
  int frameCapacity = vm.frameCapacity;
  ExceptionHandler *handlers = vm.handlers;
  int handlerCount = vm.handlerCount;
  int handlerCapacity = vm.handlerCapacity;
  ObjUpvalue *openUpvalues = vm.openUpvalues;

  vm.stack = fiber->stack;
  vm.stackTop = fiber->stackTop;
  vm.stackCapacity = fiber->stackCapacity;
  vm.frames = fiber->frames;
  vm.frameCount = fiber->frameCount;
  vm.frameCapacity = CS_FIBER_FRAMES;
  vm.handlers = fiber->handlers;
  vm.handlerCount = fiber->handlerCount;
  vm.handlerCapacity = CS_FIBER_HANDLERS;
  vm.openUpvalues = fiber->openUpvalues;

  fiber->stack = stack;
  fiber->stackTop = stackTop;
  fiber->stackCapacity = stackCapacity;
  fiber->frames = frames;
  fiber->frameCount = frameCount;
  fiber->handlers = handlers;
  fiber->handlerCount = handlerCount;
  fiber->openUpvalues = openUpvalues;
  /* The capacities the caller had are recovered from whichever kind of
   * execution state it was: the main one, or another fiber. */
  (void)frameCapacity;
  (void)handlerCapacity;
}

/* Runs `fiber` until it finishes or suspends, then puts the caller's execution
 * state back and settles the promise if it is done. */
void finishFiber(ObjFiber *fiber, ObjFiber *enclosing, InterpretResult result) {
  Value outcome = UNDEFINED_VAL;
  bool rejected = result != CS_OK;
  if (result == CS_OK) {
    outcome = csVMPop();
  } else if (vm.hasPendingException) {
    outcome = vm.pendingException;
    vm.hasPendingException = false;
  }

  ObjPromise *promise = fiber->promise;
  fiber->state = FIBER_DONE;
  swapExecutionState(fiber);
  vm.currentFiber = enclosing;

  if (promise == NULL) return;
  if (IS_OBJ(outcome)) csPushTempRoot(AS_OBJ(outcome));
  if (rejected) {
    csPromiseReject(promise, outcome);
  } else {
    csPromiseFulfill(promise, outcome);
  }
  if (IS_OBJ(outcome)) csPopTempRoot();
}

void runFiber(ObjFiber *fiber) {
  ObjFiber *enclosing = vm.currentFiber;
  vm.currentFiber = fiber;
  fiber->state = FIBER_RUNNING;
  swapExecutionState(fiber);

  vm.fiberSuspended = false;
  vm.fiberYielded = false;
  InterpretResult result = run(0);

  if (vm.fiberSuspended) {
    vm.fiberSuspended = false;
  vm.fiberYielded = false;
    fiber->state = FIBER_SUSPENDED;
    swapExecutionState(fiber);
    vm.currentFiber = enclosing;
    return;
  }
  finishFiber(fiber, enclosing, result);
}

/* Starts an async function: hands the caller a promise straight away and runs
 * the body until it either finishes or reaches its first await. */
bool callAsyncFunction(ObjClosure *closure, int argCount) {
  ObjFunction *function = closure->function;
  if (!csVMCheckArity(function, &argCount)) return false;

  ObjFiber *fiber = csFiberNew();
  csPushTempRoot((Obj *)fiber);
  fiber->promise = csPromiseNew();

  /* Slot 0 and the arguments move to the fiber's own stack; after that the
   * caller's stack is back where it was before the call. */
  Value *base = vm.stackTop - argCount - 1;
  for (int i = 0; i <= argCount; i++) fiber->stack[i] = base[i];
  fiber->stackTop = fiber->stack + argCount + 1;

  CallFrame *frame = &fiber->frames[fiber->frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = fiber->stack;

  vm.stackTop = base;
  csVMPush(OBJ_VAL(fiber->promise));

  runFiber(fiber);
  csPopTempRoot();
  return true;
}

void csVMResumeFiber(ObjFiber *fiber, Value value, bool isRejection) {
  if (fiber->state != FIBER_SUSPENDED) return;

  ObjFiber *enclosing = vm.currentFiber;
  vm.currentFiber = fiber;
  fiber->state = FIBER_RUNNING;
  swapExecutionState(fiber);

  if (isRejection) {
    /* The await throws rather than producing a value, which is what makes a
     * try/catch around one work — the handler stack belongs to this fiber. */
    CallFrame *frame = &vm.frames[vm.frameCount - 1];
    if (performThrow(value, 0, &frame) != THROW_HANDLED) {
      /* Nothing in the body caught it, so the reason travels on to the
       * promise this call handed back. performThrow only records the value
       * when it means to propagate through a return path; here the fiber ends
       * instead, so it is recorded before finishing — otherwise the rejection
       * arrives at `.catch` as undefined. */
      vm.pendingException = value;
      vm.hasPendingException = true;
      finishFiber(fiber, enclosing, CS_RUNTIME_ERROR);
      return;
    }
  } else {
    csVMPush(value);
  }

  vm.fiberSuspended = false;
  vm.fiberYielded = false;
  InterpretResult result = run(0);

  if (vm.fiberSuspended) {
    bool yielded = vm.fiberYielded;
    vm.fiberSuspended = false;
    vm.fiberYielded = false;
    fiber->state = FIBER_SUSPENDED;
    swapExecutionState(fiber);
    vm.currentFiber = enclosing;

    /* An async generator that reached a `yield` has an answer for the `next()`
     * that is still waiting on a promise. One that merely reached another
     * `await` has not, and comes back here again later. */
    if (fiber->generator != NULL) {
      csGeneratorResumed(fiber->generator, yielded, false, UNDEFINED_VAL);
    }
    return;
  }

  /* The body finished. A generator has no promise of its own to settle —
   * finishFiber would find NULL — so the outcome is taken here instead and
   * handed to whichever `next()` is waiting. */
  if (fiber->generator != NULL) {
    ObjGenerator *generator = fiber->generator;
    bool failed = result != CS_OK;
    Value reason = UNDEFINED_VAL;
    if (failed && vm.hasPendingException) {
      reason = vm.pendingException;
      vm.hasPendingException = false;
    }
    generator->yielded = !failed && vm.stackTop > vm.stack ? csVMPop() : UNDEFINED_VAL;
    generator->done = true;
    fiber->state = FIBER_DONE;
    swapExecutionState(fiber);
    vm.currentFiber = enclosing;
    csGeneratorResumed(generator, false, failed, reason);
    return;
  }

  finishFiber(fiber, enclosing, result);
}

/* ---------------- generators ---------------- */

/* Runs a generator's body until its next `yield` or its end.
 *
 * The same swap-run-swap as an async resume: what differs is only where the
 * value goes. `pushValue` is false the first time, because there is no
 * suspended `yield` waiting to receive one. */
/* Runs a generator's body until it next stops, and says how it stopped.
 *
 * `*yielded` distinguishes the two: a `yield` has a value for whoever pulled,
 * while an `await` in an async generator means the body is not finished and
 * not ready either — the event loop will bring it back. */
static InterpretResult driveGenerator(ObjGenerator *generator, bool pushValue,
                                      Value sent, bool *yielded) {
  ObjFiber *fiber = generator->fiber;
  ObjFiber *enclosing = vm.currentFiber;

  vm.currentFiber = fiber;
  fiber->state = FIBER_RUNNING;
  swapExecutionState(fiber);

  if (pushValue) csVMPush(sent);

  vm.fiberSuspended = false;
  vm.fiberYielded = false;
  InterpretResult result = run(0);

  if (vm.fiberSuspended) {
    *yielded = vm.fiberYielded;
    vm.fiberSuspended = false;
    vm.fiberYielded = false;
    fiber->state = FIBER_SUSPENDED;
    swapExecutionState(fiber);
    vm.currentFiber = enclosing;
    return CS_OK;
  }

  /* Off the end, or an explicit `return`. Either way the body is finished and
   * whatever it left on its own stack is the result. */
  *yielded = false;
  generator->yielded = result == CS_OK && vm.stackTop > vm.stack ? csVMPop()
                                                                 : UNDEFINED_VAL;
  generator->done = true;
  fiber->state = FIBER_DONE;
  swapExecutionState(fiber);
  vm.currentFiber = enclosing;
  return result;
}

/* `{ value, done }`, the shape the iterator protocol reports in. */
Value csIterationResult(Value value, bool done) {
  ObjObject *record = csObjectNew("Object");
  csPushTempRoot((Obj *)record);
  if (IS_OBJ(value)) csPushTempRoot(AS_OBJ(value));
  csObjectSetProperty(record, "value", value);
  csObjectSetProperty(record, "done", BOOL_VAL(done));
  if (IS_OBJ(value)) csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(record);
}

/* Settles the promise the last `next()` handed out, if the body has now
 * reached somewhere worth reporting. Called from both places a generator's
 * body can stop: the `next()` that started it, and the event loop resuming it
 * after an `await`. */
static void settleGeneratorStep(ObjGenerator *generator, bool yielded,
                                bool failed, Value reason) {
  ObjPromise *waiting = generator->pendingResult;
  if (waiting == NULL) return;

  /* Still awaiting: not finished, and nothing to report yet. */
  if (!yielded && !generator->done && !failed) return;

  generator->pendingResult = NULL;
  csPushTempRoot((Obj *)waiting);
  if (failed) {
    csPromiseReject(waiting, reason);
  } else {
    Value produced = generator->yielded;
    if (generator->done) generator->yielded = UNDEFINED_VAL;
    csPromiseFulfill(waiting, csIterationResult(produced, generator->done));
  }
  csPopTempRoot();
}

/* An async generator's `next()`: a promise, settled wherever the body stops. */
ObjPromise *csGeneratorNextAsync(ObjGenerator *generator, Value sent) {
  ObjPromise *result = csPromiseNew();
  csPushTempRoot((Obj *)result);

  if (generator->done || generator->running) {
    /* A finished generator answers `{ undefined, true }` for ever, and one
     * that is already running is a program error rather than a queue. */
    if (generator->running) {
      csPromiseReject(result, OBJ_VAL(csStringCopy(
          "this generator is already running", 33)));
    } else {
      csPromiseFulfill(result, csIterationResult(UNDEFINED_VAL, true));
    }
    csPopTempRoot();
    return result;
  }

  generator->pendingResult = result;

  bool started = generator->fiber->state != FIBER_READY;
  bool yielded = false;
  generator->running = true;
  InterpretResult ran = driveGenerator(generator, started, sent, &yielded);
  generator->running = false;

  if (ran != CS_OK) {
    Value reason = vm.hasPendingException ? vm.pendingException : UNDEFINED_VAL;
    vm.hasPendingException = false;
    settleGeneratorStep(generator, false, true, reason);
  } else {
    settleGeneratorStep(generator, yielded, false, UNDEFINED_VAL);
  }

  csPopTempRoot();
  return result;
}

/* The event loop brought an async generator's body back after an `await`. */
void csGeneratorResumed(ObjGenerator *generator, bool yielded, bool failed,
                        Value reason) {
  settleGeneratorStep(generator, yielded, failed, reason);
}

bool csGeneratorNext(ObjGenerator *generator, Value sent, Value *value, bool *done) {
  if (generator->done) {
    *value = UNDEFINED_VAL;
    *done = true;
    return true;
  }
  if (generator->running) {
    csVMRuntimeError("this generator is already running");
    return false;
  }

  bool started = generator->fiber->state != FIBER_READY;
  bool yielded = false;
  generator->running = true;
  InterpretResult result = driveGenerator(generator, started, sent, &yielded);
  generator->running = false;
  if (result != CS_OK) return false;

  *value = generator->yielded;
  *done = generator->done;
  /* The last value a finished body produced is its return value, and asking
   * again must not hand it out a second time. */
  if (generator->done) generator->yielded = UNDEFINED_VAL;
  return true;
}

void csGeneratorFinish(ObjGenerator *generator, Value returned) {
  generator->done = true;
  generator->yielded = returned;
  generator->fiber->state = FIBER_DONE;
}

/* Calling a generator function runs none of it: it sets the frame up on a
 * fiber of its own and hands back the handle. */
bool callGeneratorFunction(ObjClosure *closure, int argCount) {
  ObjFunction *function = closure->function;
  if (!csVMCheckArity(function, &argCount)) return false;

  ObjFiber *fiber = csFiberNew();
  csPushTempRoot((Obj *)fiber);

  /* Slot 0 and the arguments move to the fiber's own stack, exactly as an
   * async call does — a suspendable body needs slot addresses that outlive
   * the caller's stack. */
  Value *base = vm.stackTop - argCount - 1;
  for (int i = 0; i <= argCount; i++) fiber->stack[i] = base[i];
  fiber->stackTop = fiber->stack + argCount + 1;

  CallFrame *frame = &fiber->frames[fiber->frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = fiber->stack;

  ObjGenerator *generator = csGeneratorNew(fiber);
  generator->isAsync = function->isAsync;
  csPopTempRoot();

  vm.stackTop = base;
  csVMPush(OBJ_VAL(generator));
  return true;
}
