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
#define CS_MODULES_MAX 256
#define CS_NAMESPACE_MAX 256
#define CS_TIMERS_MAX 1024

/* One queued reaction. A microtask always ends by settling `result`, either
 * with what `callback` returned or — when there is no callback — with the
 * value that arrived, which is how an outcome passes through a `.then` that
 * did not ask about it. `result` is NULL for queueMicrotask. */
typedef struct {
  Value callback;
  Value argument;
  ObjPromise *result;
  bool isRejection;

  /* Set when the waiter is a suspended `await` rather than a callback. */
  struct ObjFiber *fiber;

  /* See Reaction in object.h: set when the waiter is a combinator. */
  struct ObjArray *combineState;
  int combineIndex;
} Microtask;

/* A pending setTimeout. Ties are broken by `sequence` so two timers with the
 * same delay fire in the order they were registered, which is what makes
 * output reproducible. */
typedef struct {
  double dueMs;
  long sequence;
  int id;
  Value callback;
  bool cancelled;
} Timer;

/* Defined in object.h, which includes this header for Value. */
struct ObjClosure;
struct ObjUpvalue;

/* How much of its own a suspended call carries. An async function's stack is
 * shallow in practice — it awaits from its own frame — but it may call
 * anything synchronously before it does, so the limits are real ones. */
#define CS_FIBER_STACK 1024
#define CS_FIBER_FRAMES 64
#define CS_FIBER_HANDLERS 32

/* One active call. `slots` points at the callee's window into the value stack:
 * slot 0 is the function itself, then the arguments, then its locals. That is
 * why a local's compile-time slot index is all the VM needs. */
typedef struct CallFrame {
  struct ObjClosure *closure;
  const uint8_t *ip;
  Value *slots;
} CallFrame;

/* An active `try`. Unwinding restores the machine to exactly this state, which
 * is why all three depths are recorded rather than just the resume point: a
 * throw may cross any number of calls and leave any amount of stack behind. */
typedef struct ExceptionHandler {
  int frameCount;      /* frames live when the handler was installed */
  Value *stackTop;     /* value stack depth at the same moment */
  const uint8_t *ip;   /* where to resume — the catch, or the finally */
  int handlerCount;    /* handlers below this one, for nested try blocks */
} ExceptionHandler;

typedef struct {
  /* The state of whatever is executing right now, held inline rather than
   * behind a pointer so csVMPush stays one store and the interpreter loop is
   * untouched. `await` swaps it out to a fiber and another one in; see
   * ObjFiber in object.h for why a suspended call needs its own stack rather
   * than a copy of these slots. */
  CallFrame *frames;
  int frameCount;
  int frameCapacity;

  ExceptionHandler *handlers;
  int handlerCount;
  int handlerCapacity;

  /* An exception on its way out of a nested interpreter loop.
   *
   * A native that calls back into user code runs its own run() loop. If code
   * inside that callback throws to a handler installed *outside* it, the inner
   * loop cannot unwind there — the C stack still has the native on it. So the
   * value is parked here, the inner loop returns, the native returns false,
   * and the outer loop picks the throw back up where it can be handled. */
  Value pendingException;
  bool hasPendingException;

  Value *stack;
  Value *stackTop;
  int stackCapacity;

  /* Upvalues pointing at slots that are still live on the stack, kept sorted
   * by descending slot so closing a scope can stop early. */
  struct ObjUpvalue *openUpvalues;

  /* The standard library, copied into every module as it is created. Held
   * pristine here so a module never has to fall back to a second table. */
  Table builtins;
  /* Names bound by a built-in. Used as a set; the values are ignored.
   * Assigning to anything in here is a runtime error, which is what keeps
   * `console = 1` from silently destroying the console. */
  Table builtinConsts;

  /* Every module that has been loaded, keyed by its resolved path, so
   * importing the same file twice runs it once. */
  Table modules;
  /* Where `-e`, the REPL and any code with no file of its own live. */
  ObjModule *mainModule;

  /* The fiber whose execution state is loaded right now, or NULL for the main
   * program. `await` needs it to know what to suspend. */
  ObjFiber *currentFiber;
  /* Set by OP_AWAIT so run()'s caller can tell a suspension from a return. */
  bool fiberSuspended;

  /* Modules compiled but not yet run, in dependency order. Kept alive by the
   * registry above; this only records the order. */
  ObjModule *pending[CS_MODULES_MAX];
  int pendingCount;

  Table strings; /* intern pool; weak — swept entries are removed */

  /* Built-in methods, keyed by interned name and selected by the receiver's
   * type. Arrays and strings are not property bags, so their methods cannot
   * live on the value itself the way an object's do. */
  Table arrayMethods;
  Table stringMethods;
  Table promiseMethods;

  /* The microtask queue, drained completely between macrotasks. A ring would
   * save the compaction; the queue is short-lived and this is one fewer index
   * to get wrong. */
  Microtask *microtasks;
  int microtaskCount;
  int microtaskCapacity;
  int microtaskHead;

  /* Timers, kept sorted by due time so the next one to run is always first. */
  Timer timers[CS_TIMERS_MAX];
  int timerCount;
  int nextTimerId;
  long timerSequence;

  /* Rejected promises nothing has listened to yet. Checked when the loop runs
   * dry: a rejection with no handler is a bug the program would otherwise
   * swallow. */
  ObjPromise **rejected;
  int rejectedCount;
  int rejectedCapacity;

  /* The layout every object starts from. Permanently rooted, so the shape
   * tree hanging off it is only as large as the layouts still in use — the
   * transition edges are weak and get pruned. */
  Shape *emptyShape;

  /* A shape deliberately given to no object, used as the "never filled" value
   * in an inline cache.
   *
   * NULL cannot serve: that is what a dictionary-mode object's shape is, so an
   * empty cache would report a hit on the first dictionary object to reach it
   * and then index a slot array that no longer exists. A sentinel keeps the
   * hit test at a single compare. */
  Shape *absentShape;

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

/* Marks a built-in as constant. Module-level `const` marks its own table. */
void csVMMarkBuiltinConst(ObjString *name);

/* Runs a compiled top level as a call. Used for modules and for `-e`. */
InterpretResult csVMRunBody(ObjFunction *body);

/* Runs every module compiled but not yet executed, in dependency order. */
InterpretResult csVMRunPendingModules(void);

/* Queues a reaction to run once the current call finishes. */
void csVMQueueMicrotask(Value callback, Value argument, ObjPromise *result,
                        bool isRejection);

/* Queues a Promise.all / Promise.race settlement. */
void csVMQueueCombine(struct ObjArray *state, int index, Value argument,
                      bool isRejection);

/* Drains microtasks, then timers, until neither has anything left. Reports an
 * unhandled rejection and fails if one is outstanding when it finishes. */
InterpretResult csVMRunEventLoop(void);

/* Records a rejection nothing is listening to yet. */
void csVMNoteRejection(ObjPromise *promise);

/* Continues a suspended async call with what it was waiting for, or throws
 * inside it when the promise rejected. */
void csVMResumeFiber(ObjFiber *fiber, Value value, bool isRejection);

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
