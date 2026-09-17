/* jit.h — tiering: deciding what is worth compiling, and holding what was.
 *
 * Two questions, in that order:
 *
 *   Which functions are hot?          — counted at calls and loop back-edges
 *   How much guarding would each need — which decides how far it gets
 *
 * The second is the one that matters here. CScript has optional
 * TypeScript-style annotations, and a checker that proves them before the
 * program runs. Where a function is fully annotated, a compiler can emit
 * unboxed arithmetic with no type guards and no deoptimisation points at all —
 * something a JavaScript engine can never do, because JavaScript promises
 * nothing about a value until it sees one.
 *
 * So a considered function has three possible fates, and the profile records
 * which one it got: refused outright, lowered to the typed IR — which the IR
 * interpreter can run in place of the bytecode, and does under `make test-ir`
 * — or compiled to arm64. The distance between the last two is measured
 * rather than assumed, which is how every other optimisation in this project
 * was decided.
 */
#ifndef CSCRIPT_JIT_H
#define CSCRIPT_JIT_H

#include "cscript/common.h"
#include "cscript/object.h"

/* How much work a function has to do before it is worth compiling. Calls and
 * loop back-edges both count, because a function called a million times and a
 * function called once around a million-iteration loop are equally hot.
 *
 * How much work before a function is worth compiling. Overridable so the test
 * suite can exercise the lowering: real programs cross it in a loop, but a
 * test case runs once. */
#define CS_JIT_THRESHOLD (csJitThreshold())
int csJitThreshold(void);

/* Set from the command line, before anything has had a chance to get hot.
 *
 * `CS_JIT_THRESHOLD` in the environment does the same thing and came first,
 * because the benchmark scripts set it; the flag wins where both are given,
 * on the grounds that it is the more deliberate of the two. */
void csJitSetThreshold(int threshold);

/* Puts the threshold out of reach, which is how `--no-jit` is spelled: the
 * back-edge counter still counts, because it is one compare either way, but
 * nothing ever crosses. */
void csJitDisable(void);

/* Ask for the tiering report on exit, as `CS_JIT_REPORT` in the environment
 * does. */
void csJitRequestReport(void);

typedef enum {
  JIT_INTERPRETED, /* below the threshold, or not looked at yet */
  JIT_HOT,         /* over the threshold; a backend would compile it here */
  JIT_COMPILED,    /* machine code exists — no backend yet, so unreachable */
  JIT_REFUSED,     /* hot, but holds something the backend cannot handle */
} JitState;

/* Counting a call or a back-edge, and tiering up on the way past the
 * threshold.
 *
 * Measured before being switched on: the back-edge counter costs 4.8% on
 * `loop_arith` and 2.8% on `locals`, because it is two loads, an increment and
 * a compare on every iteration of every loop. That is a fair price for a
 * compiler that pays it back and no price at all worth paying for
 * instrumentation, so it is built only into the `jit` configuration until
 * there is a backend to earn it. The number is recorded because it is a real
 * input to the decision: a code generator has to beat 5% on tight loops before
 * it is even break-even.
 */
#ifdef CS_DEBUG_JIT
#define CS_JIT_TICK(fn)                                         \
  do {                                                          \
    if (++(fn)->hotness == CS_JIT_THRESHOLD) csJitConsider(fn); \
  } while (false)
#else
#define CS_JIT_TICK(fn) ((void)0)
#endif

/* Called when a function crosses the threshold. Scans, lowers and compiles as
 * far as the function allows, and records what happened either way — a refusal
 * is as much of a result as a compile, and `--jit-report` prints both. */
void csJitConsider(ObjFunction *function);

/* Marks what compiled code assumes about object layouts. jit.c is in every
 * build, so this is too — it simply has nothing to mark where no function was
 * ever considered. */
void csJitMarkRoots(void);

/* Runs a hot function's lowered IR in place of its bytecode, when it has any
 * and the IR covers what it does.
 *
 * This is how the lowering is verified. Rather than run both and compare, the
 * IR simply *replaces* the interpreter for the functions it can handle — which
 * makes the existing golden suite the check: 78 cases and 14 Node-parity
 * examples all have to keep producing the same output. A mistranslation shows
 * up as a failing test rather than as a number that is quietly wrong.
 *
 * Abandoning halfway is safe: the IR interpreter touches only its own slots
 * and registers, so a function it cannot finish leaves nothing behind and the
 * bytecode runs as usual.
 *
 * `receiver` is what slot 0 holds: a method's `this`, or whatever an ordinary
 * call would have left in the callee's own slot. Passing it rather than
 * assuming undefined is what lets a method be answered from compiled code —
 * the entry checks read slot 0 like any other, and a method's property reads
 * are mostly through it. */
/* What became of a call the compiler was offered. */
typedef enum {
  /* Compiled code did not run. Interpret the call, as if it had never been
   * offered — by far the common answer, and the cheap one. */
  JIT_RUN_DECLINED,

  /* Compiled code ran the whole call and `out` holds the result. */
  JIT_RUN_ANSWERED,

  /* Compiled code gave up part-way and **a frame has been pushed** for the
   * interpreter to resume in, at the offset the compiled code stopped at. The
   * caller does nothing but let its dispatch loop pick the new frame up. */
  JIT_RUN_DEOPTIMISED,

  /* A call the compiled code made threw. The frame is over and the exception
   * is pending: the caller must *not* interpret the function again, because
   * whatever the callee did, it did. */
  JIT_RUN_FAILED,
} JitRunResult;

JitRunResult csJitTryRun(ObjClosure *closure, Value receiver, const Value *args, int argCount, Value *out);

/* Takes over a loop that is already running, at `bytecodeOffset`.
 *
 * Called from the back-edge that just counted, and the reason the back-edge
 * counter exists at all: the function it makes hot is by definition already
 * executing, so there is no next call to enter compiled code from. Returns
 * false — cheaply, and by far the common case — when this function has no
 * compiled entry for this offset.
 *
 * `slots` is the interpreter's own frame. The compiled code reads and writes
 * it in place, which is what makes the hand-over free.
 *
 * On success one of two things happened. `*resumeAt` is -1 when the function
 * ran to completion and `out` holds its result. Otherwise the compiled code
 * reached something it does not implement and handed the frame back: resume
 * the bytecode at `*resumeAt` with the operand stack `*resumeHeight` deep.
 *
 * `failed` is the third case, and the reason it is not one of the other two: a
 * call the compiled loop made threw. The loop is over and its frame is not
 * resumable at any offset, so the interpreter takes the throw instead. */
bool csJitOsr(ObjFunction *function, int bytecodeOffset, Value *slots, Value *out, int *resumeAt, int *resumeHeight, bool *failed);

/* Why a function was refused, or NULL when it was not. */
const char *csJitRefusalReason(const ObjFunction *function);

/* Prints what tiering saw. Built into the `jit` configuration only. */
void csJitDumpProfile(void);

#endif /* CSCRIPT_JIT_H */
