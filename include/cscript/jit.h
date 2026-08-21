/* jit.h — tiering: deciding what is worth compiling, and holding what was.
 *
 * This is the plumbing a just-in-time compiler needs before it has a code
 * generator. It answers two questions and nothing else yet:
 *
 *   Which functions are hot?          — counted at calls and loop back-edges
 *   Which of those could be compiled  — and, crucially, *how much guarding*
 *   without guarding everything?        each would need
 *
 * The second question is the one worth asking here. CScript has optional
 * TypeScript-style annotations, and a checker that proves them before the
 * program runs. Where a function is fully annotated, a compiler can emit
 * unboxed arithmetic with no type guards and no deoptimisation points at all —
 * something a JavaScript engine can never do, because JavaScript promises
 * nothing about a value until it sees one.
 *
 * So the profile records, per function, how many operations the compiler could
 * already specialise from a declared type against how many it had to leave
 * generic. That ratio decides whether a type-directed compiler is worth
 * building, and it is measured rather than assumed — which is how every other
 * optimisation in this project was decided.
 */
#ifndef CSCRIPT_JIT_H
#define CSCRIPT_JIT_H

#include "cscript/common.h"
#include "cscript/object.h"

/* How much work a function has to do before it is worth compiling. Calls and
 * loop back-edges both count, because a function called a million times and a
 * function called once around a million-iteration loop are equally hot. */
/* How much work before a function is worth compiling. Overridable so the test
 * suite can exercise the lowering: real programs cross it in a loop, but a
 * test case runs once. */
#define CS_JIT_THRESHOLD (csJitThreshold())
int csJitThreshold(void);

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
#define CS_JIT_TICK(fn)                                        \
  do {                                                         \
    if (++(fn)->hotness == CS_JIT_THRESHOLD) csJitConsider(fn); \
  } while (false)
#else
#define CS_JIT_TICK(fn) ((void)0)
#endif

/* Called when a function crosses the threshold. With no code generator behind
 * it this only records the decision, which is the point of the stage: the
 * decision is what has to be shown to be right before the generator exists. */
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
 * bytecode runs as usual. */
bool csJitTryRun(ObjFunction *function, const Value *args, int argCount, Value *out);

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
 * the bytecode at `*resumeAt` with the operand stack `*resumeHeight` deep. */
bool csJitOsr(ObjFunction *function, int bytecodeOffset, Value *slots, Value *out,
              int *resumeAt, int *resumeHeight);

/* Why a function was refused, or NULL when it was not. */
const char *csJitRefusalReason(const ObjFunction *function);

/* Prints what tiering saw. Built into the `jit` configuration only. */
void csJitDumpProfile(void);

#endif /* CSCRIPT_JIT_H */
