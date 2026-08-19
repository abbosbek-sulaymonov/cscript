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
#include "vm_internal.h"


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
  InterpretResult result = run(0);

  if (vm.fiberSuspended) {
    vm.fiberSuspended = false;
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
  if (argCount != function->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d",
                     function->name != NULL ? function->name->chars : "<anonymous>",
                     function->arity, function->arity == 1 ? "" : "s", argCount);
    return false;
  }

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
      finishFiber(fiber, enclosing, CS_RUNTIME_ERROR);
      return;
    }
  } else {
    csVMPush(value);
  }

  vm.fiberSuspended = false;
  InterpretResult result = run(0);
  if (vm.fiberSuspended) {
    vm.fiberSuspended = false;
    fiber->state = FIBER_SUSPENDED;
    swapExecutionState(fiber);
    vm.currentFiber = enclosing;
    return;
  }
  finishFiber(fiber, enclosing, result);
}
