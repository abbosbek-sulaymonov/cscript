/* vm.h — the stack machine that executes compiled chunks.
 *
 * There is one global VM. It owns every heap object, the intern pool and the
 * globals map, which is also what makes it the collector's root set.
 */
#ifndef CSCRIPT_VM_H
#define CSCRIPT_VM_H

#include "cscript/chunk.h"
#include "cscript/common.h"
#include "cscript/table.h"
#include "cscript/value.h"

typedef enum {
  CS_OK,
  CS_COMPILE_ERROR,
  CS_RUNTIME_ERROR,
} InterpretResult;

#define CS_TEMP_ROOTS_MAX 64
#define CS_FRAMES_MAX 64
#define CS_HANDLERS_MAX 64

/* Defined in object.h, which includes this header for Value. */
struct ObjClosure;
struct ObjUpvalue;

/* One active call. `slots` points at the callee's window into the value stack:
 * slot 0 is the function itself, then the arguments, then its locals. That is
 * why a local's compile-time slot index is all the VM needs. */
typedef struct {
  struct ObjClosure *closure;
  const uint8_t *ip;
  Value *slots;
} CallFrame;

/* An active `try`. Unwinding restores the machine to exactly this state, which
 * is why all three depths are recorded rather than just the resume point: a
 * throw may cross any number of calls and leave any amount of stack behind. */
typedef struct {
  int frameCount;      /* frames live when the handler was installed */
  Value *stackTop;     /* value stack depth at the same moment */
  const uint8_t *ip;   /* where to resume — the catch, or the finally */
  int handlerCount;    /* handlers below this one, for nested try blocks */
} ExceptionHandler;

typedef struct {
  CallFrame frames[CS_FRAMES_MAX];
  int frameCount;

  ExceptionHandler handlers[CS_HANDLERS_MAX];
  int handlerCount;

  /* An exception on its way out of a nested interpreter loop.
   *
   * A native that calls back into user code runs its own run() loop. If code
   * inside that callback throws to a handler installed *outside* it, the inner
   * loop cannot unwind there — the C stack still has the native on it. So the
   * value is parked here, the inner loop returns, the native returns false,
   * and the outer loop picks the throw back up where it can be handled. */
  Value pendingException;
  bool hasPendingException;

  Value stack[CS_STACK_MAX];
  Value *stackTop;

  /* Upvalues pointing at slots that are still live on the stack, kept sorted
   * by descending slot so closing a scope can stop early. */
  struct ObjUpvalue *openUpvalues;

  Table globals;
  /* Names bound by `const` or by a built-in. Used as a set; the values are
   * ignored. Assigning to anything in here is a runtime error, which is what
   * keeps `console = 1` from silently destroying the console. */
  Table globalConsts;
  Table strings; /* intern pool; weak — swept entries are removed */

  /* Built-in methods, keyed by interned name and selected by the receiver's
   * type. Arrays and strings are not property bags, so their methods cannot
   * live on the value itself the way an object's do. */
  Table arrayMethods;
  Table stringMethods;

  Obj *objects; /* head of the intrusive list of every live object */

  /* Tri-colour marking worklist. Deliberately allocated with raw realloc so a
   * collection can never recurse into itself. */
  int grayCount;
  int grayCapacity;
  Obj **grayStack;

  size_t bytesAllocated;
  size_t nextGC;

  Obj *tempRoots[CS_TEMP_ROOTS_MAX];
  int tempRootCount;

  const char *sourceName;
} VM;

extern VM vm;

void csVMInit(void);
void csVMFree(void);

/* Compiles and runs a whole source string. */
InterpretResult csInterpret(const char *source, const char *sourceName);

/* Marks a global as constant. Used by `const` and by the built-ins. */
void csVMMarkGlobalConst(ObjString *name);

void csVMPush(Value value);
Value csVMPop(void);
Value csVMPeek(int distance);

/* Calls a CScript value from inside a native, running a nested interpreter loop
 * until the call returns. This is what lets `map`, `filter`, `reduce` and a
 * `sort` comparator invoke user code.
 *
 * The arguments must already be pushed, `callee` first. On success the result
 * is written to `result` and the callee and arguments are gone from the stack;
 * on failure the error has been reported and the caller should return false.
 *
 * Depth is bounded by CS_FRAMES_MAX, the same limit ordinary calls obey, so
 * this adds no new way to run out of stack. */
bool csVMCallCallback(Value callee, int argCount, Value *result);

/* Calls `callee` with as many of `args` as it actually declares, padding with
 * undefined if it declares more.
 *
 * Direct calls keep strict arity, because calling a function with the wrong
 * number of arguments is nearly always a bug. That rule cannot hold for
 * callbacks: `map` offers (element, index, array) and almost every callback
 * wants only the first. JavaScript solves this by making every call variadic;
 * CScript instead makes the *caller* adapt, so the strict check still protects
 * ordinary code. */
bool csVMCallAdapted(Value callee, Value *args, int available, Value *result);

/* Reports a runtime error at the currently executing instruction and unwinds.
 * Native functions call this before returning false. */
void csVMRuntimeError(const char *format, ...);

#endif /* CSCRIPT_VM_H */
