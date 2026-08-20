/* vm_internal.h — the seams between the VM's three translation units.
 *
 * vm.c holds the interpreter loop and everything on its hot path. The loop is
 * one function on purpose: computed-goto dispatch depends on the labels and
 * the cached `ip`/`frame` living in a single frame, and the helpers it calls
 * on every instruction have to stay in the same translation unit to be
 * inlined. Splitting it would cost exactly the speed the dispatch is for.
 *
 * What could be moved out was: the event loop, which runs only after the
 * program does, and fibers, which run only when an async call suspends.
 * Neither is on the hot path.
 */
#ifndef CSCRIPT_VM_INTERNAL_H
#define CSCRIPT_VM_INTERNAL_H

#include "cscript/common.h"
#include "cscript/object.h"
#include "cscript/vm.h"


typedef enum {
  THROW_HANDLED,   /* a handler in this loop took it; keep executing */
  THROW_PROPAGATE, /* the handler belongs to an outer loop */
  THROW_UNCAUGHT,  /* nothing anywhere will take it */
} ThrowResult;

/* vm.c — the interpreter loop and the machinery on its hot path */
void resetStack(void);
bool callClosure(ObjClosure *closure, int argCount);
bool callNative(ObjNative *native, Value receiver, int argCount);
bool callValue(Value callee, int argCount);
ThrowResult performThrow(Value thrown, int baseFrame, CallFrame **frame);
InterpretResult run(int baseFrame);
void csVMPush(Value value);
Value csVMPop(void);
bool runFieldInitializers(ObjClass *klass, ObjClass *stopAt, Value instance);
ObjClosure *findConstructor(ObjClass *klass);

/* src/vm_event.c */
void csVMQueueMicrotask(Value callback, Value argument, ObjPromise *result,
                        bool isRejection);
Microtask *csVMLastMicrotask(void);
void runCombine(const Microtask *task);
void csVMNoteRejection(ObjPromise *promise);
double nowMs(void);
InterpretResult runMicrotask(const Microtask *task);
InterpretResult drainMicrotasks(void);
InterpretResult reportUnhandledRejections(void);
InterpretResult csVMRunEventLoop(void);

/* src/vm_fiber.c */
void swapExecutionState(ObjFiber *fiber);
void finishFiber(ObjFiber *fiber, ObjFiber *enclosing, InterpretResult result);
void runFiber(ObjFiber *fiber);
bool callAsyncFunction(ObjClosure *closure, int argCount);

/* Builds a generator for a `function*` call, without running any of it. */
bool callGeneratorFunction(ObjClosure *closure, int argCount);

/* Runs a generator to its next `yield` or its end. `sent` becomes the value
 * the suspended `yield` produces. Returns false only when the body failed,
 * which has already been reported. */
bool csGeneratorNext(ObjGenerator *generator, Value sent, Value *value, bool *done);

/* `next()` on an async generator: a promise for the next `{ value, done }`. */
ObjPromise *csGeneratorNextAsync(ObjGenerator *generator, Value sent);

/* Settles a pending `next()` after the event loop resumed the body. */
void csGeneratorResumed(ObjGenerator *generator, bool yielded, bool failed,
                        Value reason);

/* `{ value, done }`. */
Value csIterationResult(Value value, bool done);

/* Marks a generator finished without running any more of it, for `.return()`. */
void csGeneratorFinish(ObjGenerator *generator, Value returned);
void csVMResumeFiber(ObjFiber *fiber, Value value, bool isRejection);

#endif /* CSCRIPT_VM_INTERNAL_H */
