/* jit.c — tiering: what gets compiled, and what holds what was.
 *
 * A function that crosses the threshold is considered once, and everything
 * that can happen to it happens here: its bytecode is scanned for anything a
 * backend would refuse, it is lowered to the typed IR, the IR's passes run,
 * and — where every arithmetic operand is proved a number — it is compiled to
 * machine code.
 *
 * The refusals are kept beside the successes deliberately. Lowering is
 * attempted even for a function the scan would decline, because the two
 * refusals answer different questions, and the gap between them is the work
 * still to do. `--jit-report` prints it.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/debug.h"
#include "cscript/ir.h"
#include "cscript/jitcode.h"
#include "cscript/jit.h"
#include "cscript/memory.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/vm.h"

/* Every function that got hot, in the order it did. Bounded and never grown:
 * this is a diagnostic, and a program with more than this many hot functions
 * has already told us what we wanted to know. */
#define CS_JIT_MAX_HOT 256

typedef struct {
  ObjFunction *function;
  const char *refusal;   /* why a backend would skip it, or NULL */
  const char *irRefusal; /* why it would not lower, or NULL */
  IrFunction *ir;        /* the lowered form, when it lowered */
  JitCode *code;         /* machine code, when it compiled */
  const char *codeRefusal;
  Value *scratch; /* the compiled code's working space */

  /* Decided once, when the IR is lowered. Both were being recomputed on every
   * call — and csIrIsFullyTyped walks every instruction of every block, so a
   * function answered by compiled code was paying a walk of its own program
   * per call. That is what made answering a call no faster than interpreting
   * one. */
  bool entryUsable;
} HotEntry;

static HotEntry hot[CS_JIT_MAX_HOT];
static int hotCount = 0;

/* How many calls the IR actually answered, so the verification can be shown
 * not to be vacuous: a gate that never opens proves nothing. */
static long substituted = 0;

/* And how many loops were taken over mid-flight, which is the separate
 * question: a compiled function a loop benchmark never enters is a compiler
 * that measured nothing. */
static long osrEntered = 0;

/* And how many of those handed the frame back part-way rather than finishing
 * it, which is the measure of how much of a program the compiler is actually
 * taking on. */
static long exited = 0;

/* Of those, how many were entered by a *call* — where handing the frame back
 * means building one. Counted apart because it is the thing that says a body
 * with an exit in it is being entered at all rather than refused. */
static long deoptimised = 0;

/* Opcodes a first backend would not attempt.
 *
 * Not a permanent list — it is what the *first* code generator would leave to
 * the interpreter, either because it suspends a frame, because it can throw
 * past one, or because it calls back into the VM in a way a compiled frame
 * would have to model. Everything else is arithmetic, moves and branches. */
static const char *unsupportedOpcode(uint8_t opcode) {
  switch (opcode) {
    case OP_AWAIT: return "suspends the frame (await)";
    case OP_TRY:
    case OP_THROW:
    case OP_END_TRY: return "unwinds past the frame (try/throw)";
    case OP_NEW:
    case OP_SUPER_CALL:
    case OP_SUPER_INVOKE:
    case OP_GET_SUPER: return "constructs or calls through a class";
    case OP_IMPORT_NAME:
    case OP_IMPORT_NAMESPACE: return "resolves a module";
    case OP_CLASS:
    case OP_INHERIT:
    case OP_METHOD:
    case OP_STATIC_METHOD:
    case OP_STATIC_FIELD:
    case OP_CONSTRUCTOR:
    case OP_FIELD_INIT:
    case OP_GETTER:
    case OP_SETTER: return "builds a class";
    default: return NULL;
  }
}

/* Walks the chunk once looking for anything the backend could not emit.
 *
 * Instruction lengths differ, so this decodes rather than striding: getting
 * that wrong would read an operand as an opcode and refuse for a reason that
 * is not there. */
static const char *scanChunk(const Chunk *chunk) {
  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    const char *reason = unsupportedOpcode(opcode);
    if (reason != NULL) return reason;
    offset = csInstructionLength(chunk, offset);
    if (offset <= 0) return "could not be decoded";
  }
  return NULL;
}

/* Resolved once, from the flag if one was given, then the environment, then
 * the default. Held rather than recomputed because the back-edge counter asks
 * for it on every iteration of every loop. */
static int jitThreshold = -1;
static bool jitReportRequested = false;

int csJitThreshold(void) {
  if (jitThreshold < 0) {
    const char *override = getenv("CS_JIT_THRESHOLD");
    jitThreshold = override != NULL ? atoi(override) : 10000;
    if (jitThreshold < 1) jitThreshold = 1;
  }
  return jitThreshold;
}

void csJitSetThreshold(int threshold) {
  jitThreshold = threshold < 1 ? 1 : threshold;
}

void csJitDisable(void) {
  jitThreshold = INT_MAX;
}

void csJitRequestReport(void) {
  jitReportRequested = true;
}

/* Do the arguments still match what the lowering was told to expect? Only the
 * parameters it speculated on are checked; an annotated one was already
 * checked at the call boundary, and an unspeculated one was never assumed. */
static bool observedTypesHold(const ObjFunction *function, const Value *args, int argCount) {
  if (function->observedParams == NULL) return true;

  for (int i = 0; i < function->paramCount; i++) {
    if (function->observedParams[i] != CS_PARAM_NUMBER) continue;
    /* A parameter the call left out gets its default, which the compiled
     * prologue produces; there is no argument to check. */
    if (i >= argCount) continue;
    if (!IS_NUMBER(args[i])) return false;
  }
  return true;
}

/* The same question asked of a frame rather than of an argument list, for the
 * OSR path — where the parameters are already in their slots.
 *
 * Both paths need it and only one had it, which is exactly the shape of the
 * bug that produced: compiled code keeps a slot it believes is a number in a
 * floating-point register, so entering a loop whose parameter turned out to
 * hold a pointer read that pointer as a double and wrote the result back. */
static bool observedTypesHoldInFrame(const ObjFunction *function, const Value *slots) {
  if (function->observedParams == NULL) return true;

  for (int i = 0; i < function->paramCount; i++) {
    if (function->observedParams[i] != CS_PARAM_NUMBER) continue;
    if (!IS_NUMBER(slots[i + 1])) return false;
  }
  return true;
}

/* Whether the assumptions the compiled code was built on still hold.
 *
 * Two of them. The addresses it holds for globals point into a hash table, and
 * are only meaningful while that table has not rehashed — the version says.
 * And every one of those globals was a number when the code was built, which
 * is what let the arithmetic be compiled with no guard around it; the language
 * will not let a declared binding change type, but an undeclared one reached
 * through a namespace could, so it is checked rather than assumed.
 *
 * Checked once on the way in rather than at every access. Nothing inside a
 * compiled region can call anything, so nothing can invalidate either between
 * the check and the end of the run. */
static bool assumptionsHold(const JitCode *code) {
  if (code->globalCount == 0) return true;
  if (code->globalTable == NULL) return false;
  if (code->globalTable->version != code->globalVersion) return false;

  for (int g = 0; g < code->globalCount; g++) {
    if (!IS_NUMBER(*code->globalAddress[g])) return false;
  }
  return true;
}

/* Whether a deoptimising entry can build the frame the interpreter will
 * resume in.
 *
 * The frame has to be the one the call would have got, and `csVMCheckArity`
 * builds that by *padding*: a missing argument becomes undefined, a rest
 * parameter becomes an array, and a default is filled in by a prologue at the
 * top of the body. All three happen before the offset a deoptimising exit
 * resumes at, so a call that needs any of them would have them done twice.
 * Exactly as many arguments as parameters, and none of it arises. */
static bool deoptimisableCall(const ObjFunction *function, int argCount) {
  return argCount == function->arity && !function->hasRest && function->paramCount == function->arity;
}

JitRunResult csJitTryRun(ObjClosure *closure, Value receiver, const Value *args, int argCount, Value *out) {
  ObjFunction *function = closure->function;
  /* Both states are runnable: JIT_HOT has lowered IR, JIT_COMPILED also has
   * machine code. Admitting only the first rejected exactly the functions that
   * had got furthest. */
  if (function->jitState != JIT_HOT && function->jitState != JIT_COMPILED) return JIT_RUN_DECLINED;
  /* The entry recorded on the function itself, so a call costs no search. */
  int index = function->jitSlot;
  if (index >= 0 && index < hotCount && hot[index].function == function) {
    if (!hot[index].entryUsable) return JIT_RUN_DECLINED;
    int i = index;

    /* A body with an exit in it can be entered, and leaves through the exit
     * into a frame built on the way out — but only where that frame is the one
     * the call would have got anyway. */
    bool mayDeoptimise = hot[i].code != NULL && deoptimisableCall(function, argCount);
    if (hot[i].ir->hasExits && !mayDeoptimise) return JIT_RUN_DECLINED;

    /* Only while what was *observed* still holds. A parameter the lowering
     * took to be a number on the strength of the calls it had seen is checked
     * here, every time — that is the difference between an observation and the
     * annotation next to it, and the reason speculating is safe. */
    if (!observedTypesHold(hot[i].function, args, argCount)) return JIT_RUN_DECLINED;

    /* The frame, built once.
     *
     * It used to be built twice — once to check the layouts against and once
     * to run on — which cost more per call than interpreting the call did, and
     * left a method answered three million times by compiled code exactly as
     * fast as one that was not. */
    if (hot[i].ir->slotCount > 256) return JIT_RUN_DECLINED;
    Value slots[256];
    for (int s = 0; s < hot[i].ir->slotCount; s++) slots[s] = UNDEFINED_VAL;
    if (hot[i].ir->slotCount > 0) slots[0] = receiver;
    for (int a = 0; a < argCount && a + 1 < hot[i].ir->slotCount; a++) {
      slots[a + 1] = args[a];
    }

    /* The layouts the property reads were lowered against, asked of the frame
     * they will actually run on. */
    if (hot[i].ir->entryShapeCount > 0 && !csIrEntryShapesHold(hot[i].ir, slots)) {
      return JIT_RUN_DECLINED;
    }

    /* And the bindings the inlined bodies came from, which have to still mean
     * the functions whose bodies those are. */
    if (hot[i].ir->inlinedCount > 0 && !csIrInlinedCalleesHold(hot[i].ir)) {
      return JIT_RUN_DECLINED;
    }

    /* Compiled code, when there is any. */
    if (hot[i].code != NULL) {
      if (!assumptionsHold(hot[i].code)) return JIT_RUN_DECLINED;

      /* The frame is a local array, which the collector has no way to find.
       * Told about it for exactly as long as the run lasts — anything compiled
       * code allocates is stored into one of these slots before anything else
       * can collect. */
      /* Announced for the length of the run, and *pushed*: a compiled function
       * that calls another one leaves this frame live while the inner run adds
       * its own. Replacing rather than pushing would unroot the caller. */
      if (vm.jitRootRanges >= CS_JIT_ROOT_MAX) return JIT_RUN_DECLINED;
      int savedRanges = vm.jitRootRanges;
      vm.jitRoots[vm.jitRootRanges].values = slots;
      vm.jitRoots[vm.jitRootRanges].count = hot[i].ir->slotCount;
      vm.jitRootRanges++;

      int exit = -1;
      uint64_t bits = hot[i].code->entry(slots, hot[i].scratch, &exit);

      if (exit >= 0) {
        if (exit >= hot[i].code->exitCount) {
          vm.jitRootRanges = savedRanges;
          return JIT_RUN_DECLINED;
        }

        /* A call the compiled code made threw. That must not send the caller
         * back to interpret this function: the callee already ran. */
        if (hot[i].code->exits[exit].bytecodeOffset == CS_JIT_EXIT_FAILED) {
          vm.jitRootRanges = savedRanges;
          return JIT_RUN_FAILED;
        }

        /* An ordinary exit: the compiled code reached something it does not
         * implement. Nothing between the exit's offset and here ran — the
         * lowering drops everything it emitted past that point — so the
         * interpreter picks the function up there, on a frame holding what
         * compiled code has computed so far.
         *
         * The frame stays announced until its values have been copied into the
         * one the interpreter will use: pushing a frame is allowed to allocate,
         * and until the copy is done this array is the only thing holding what
         * compiled code built. */
        bool moved = csVMDeoptimise(closure, argCount, slots, hot[i].ir->slotCount, hot[i].code->exits[exit].bytecodeOffset, hot[i].code->exits[exit].stackHeight);
        vm.jitRootRanges = savedRanges;
        if (!moved) return JIT_RUN_FAILED;
        exited++;
        deoptimised++;
        return JIT_RUN_DEOPTIMISED;
      }

      vm.jitRootRanges = savedRanges;
      memcpy(out, &bits, sizeof(Value));
      substituted++;
      return JIT_RUN_ANSWERED;
    }
    if (!csIrInterpret(hot[i].ir, args, argCount, out)) return JIT_RUN_DECLINED;
    substituted++;
    return JIT_RUN_ANSWERED;
  }
  return JIT_RUN_DECLINED;
}

bool csJitOsr(ObjFunction *function, int bytecodeOffset, Value *slots, Value *out, int *resumeAt, int *resumeHeight, bool *failed) {
  *resumeAt = -1;
  *failed = false;
  if (function->jitOsrRefusedAt == bytecodeOffset) return false;

  /* The entry recorded on the function itself, so a back-edge costs no search
   * — the same fix the call path already had. */
  int index = function->jitSlot;
  if (index >= 0 && index < hotCount && hot[index].function == function) {
    int i = index;
    if (hot[i].code == NULL) {
      function->jitOsrRefusedAt = bytecodeOffset;
      return false;
    }

    for (int o = 0; o < hot[i].code->osrCount; o++) {
      if (hot[i].code->osr[o].bytecodeOffset != bytecodeOffset) continue;

      /* The compiled code writes every slot the IR knows about, including any
       * belonging to a scope the interpreter has not opened yet. Those sit
       * above the frame's current top, so the room has to be there. */
      if (slots + hot[i].ir->slotCount >= vm.stack + vm.stackCapacity) return false;
      if (!assumptionsHold(hot[i].code)) return false;
      /* The parameters this code was lowered on the strength of are still in
       * their slots, and still have to be what they were guessed to be. */
      if (!observedTypesHoldInFrame(function, slots)) return false;
      if (!csIrEntryShapesHold(hot[i].ir, slots)) return false;
      if (!csIrInlinedCalleesHold(hot[i].ir)) return false;

      /* The slots above what the interpreter has opened, which the collector
       * walks only as far as `stackTop`. The frame below it is already a root;
       * this covers the rest for as long as compiled code is using it. */
      if (vm.jitRootRanges >= CS_JIT_ROOT_MAX) return false;
      int savedRanges = vm.jitRootRanges;
      vm.jitRoots[vm.jitRootRanges].values = slots;
      vm.jitRoots[vm.jitRootRanges].count = hot[i].ir->slotCount;
      vm.jitRootRanges++;

      int exit = -1;
      uint64_t bits = hot[i].code->osr[o].entry(slots, hot[i].scratch, &exit);

      vm.jitRootRanges = savedRanges;

      if (exit >= 0 && exit < hot[i].code->exitCount) {
        /* A call this loop made threw. There is no offset to resume at — the
         * callee ran, and re-entering the loop would run it again — so this
         * says so rather than handing back a frame. */
        if (hot[i].code->exits[exit].bytecodeOffset == CS_JIT_EXIT_FAILED) {
          *failed = true;
          return false;
        }
        /* Not finished: the body reached something the compiler does not
         * implement, and the interpreter takes it from there. */
        *resumeAt = hot[i].code->exits[exit].bytecodeOffset;
        *resumeHeight = hot[i].code->exits[exit].stackHeight;
        exited++;
        return true;
      }

      memcpy(out, &bits, sizeof(Value));
      osrEntered++;
      return true;
    }
    return false;
  }
  return false;
}

/* What every compiled body is holding on to: the shapes its property accesses
 * were lowered against, and the closures whose bodies were spliced into it.
 * They are ordinary collectable objects, and a shape's transition edges are
 * weak — so without this a shape nothing else referred to could be freed and
 * its memory come back as a different shape, which would make an entry check
 * pass exactly when it must fail. An inlined callee is the same hazard: rebind
 * the global it came from and the entry check is the only reference left. */
void csJitMarkRoots(void) {
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir != NULL) csIrMarkReferences(hot[i].ir);
  }
}

void csJitConsider(ObjFunction *function) {
  if (function->jitState != JIT_INTERPRETED) return;

  const char *refusal = scanChunk(&function->chunk);
  function->jitState = refusal == NULL ? JIT_HOT : JIT_REFUSED;

  if (hotCount >= CS_JIT_MAX_HOT) return;

  hot[hotCount].function = function;
  hot[hotCount].refusal = refusal;
  hot[hotCount].ir = NULL;
  hot[hotCount].irRefusal = NULL;

  /* Lowering is attempted even for a function a backend would skip: the two
   * refusals answer different questions, and the gap between them is the
   * work still to do. */
  const char *why = NULL;
  hot[hotCount].ir = csIrLower(function, &why);
  hot[hotCount].irRefusal = why;
  if (hot[hotCount].ir != NULL) {
    /* Forward first: it turns loads into renames, which is what leaves the
     * stores behind for the second pass to find. */
    /* Before anything trusts a type: the lowering's are provisional. */
    csIrReconcileSlotTypes(hot[hotCount].ir);
    csIrForwardSlots(hot[hotCount].ir);
    csIrRemoveDeadStores(hot[hotCount].ir);
    csIrReconcileSlotTypes(hot[hotCount].ir);
    if (getenv("CS_JIT_DUMP_IR") != NULL) csIrPrint(hot[hotCount].ir);
  }
  hot[hotCount].code = NULL;
  hot[hotCount].codeRefusal = NULL;
  hot[hotCount].scratch = NULL;

  /* Decided here, once. A body with an exit in it is admitted now: the exit
   * builds the frame the interpreter resumes in — see csVMDeoptimise — where
   * before it meant the call entry was refused outright. The IR interpreter
   * still cannot take one, and csJitTryRun keeps that apart. */
  hot[hotCount].entryUsable = hot[hotCount].ir != NULL && csIrIsFullyTyped(hot[hotCount].ir);
  function->jitSlot = hotCount;

  /* Machine code only where every arithmetic operand is proved a number.
   * Nothing else is attempted, because anything else would need a guard. */
  if (hot[hotCount].ir != NULL && csIrIsFullyTyped(hot[hotCount].ir)) {
    const char *codeWhy = NULL;
    hot[hotCount].code = csJitCompile(hot[hotCount].ir, &codeWhy);
    hot[hotCount].codeRefusal = codeWhy;
    if (hot[hotCount].code != NULL) {
      hot[hotCount].scratch = (Value *)calloc((size_t)hot[hotCount].ir->registerCount + 1, sizeof(Value));
      /* Stored as data, because ISO C does not promise a function pointer
       * fits in a void*. It is only ever read back through the same cast. */
      memcpy(&function->jitCode, &hot[hotCount].code->entry, sizeof(void *));
      function->jitState = JIT_COMPILED;
    }
  }

  hotCount++;
}

const char *csJitRefusalReason(const ObjFunction *function) {
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].function == function) return hot[i].refusal;
  }
  return NULL;
}

void csJitDumpProfile(void) {
  /* Asked for explicitly. The report goes to stdout, so printing it by default
   * would land in the middle of whatever the program itself wrote — including
   * every golden test, which is how the lowering is verified. */
  if (!jitReportRequested && getenv("CS_JIT_REPORT") == NULL) return;

  if (hotCount == 0) {
    printf("\n== tiering: nothing reached %d ==\n", CS_JIT_THRESHOLD);
    return;
  }

  printf("\n== tiering: %d function%s over %d ==\n\n", hotCount, hotCount == 1 ? "" : "s", CS_JIT_THRESHOLD);
  printf("  %-28s %10s %8s %8s  %s\n", "function", "hotness", "typed", "generic", "verdict");

  int compilable = 0;
  int typedTotal = 0;
  int genericTotal = 0;

  for (int i = 0; i < hotCount; i++) {
    ObjFunction *function = hot[i].function;
    const char *name = function->name != NULL ? function->name->chars : "<top level>";

    typedTotal += function->typedSites;
    genericTotal += function->genericSites;
    if (hot[i].refusal == NULL) compilable++;

    printf("  %-28.28s %10d %8d %8d  %s\n", name, function->hotness, function->typedSites, function->genericSites,
           hot[i].refusal != NULL ? hot[i].refusal : "compilable");
  }

  int lowered = 0;
  int typedRegisters = 0;
  int totalRegisters = 0;
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL) continue;
    lowered++;
    for (int r = 0; r < hot[i].ir->registerCount; r++) {
      totalRegisters++;
      if (hot[i].ir->registerTypes[r] == IR_TYPE_NUMBER) typedRegisters++;
    }
  }

  /* Why the ones that did not, and where the rest stopped short. A count of
   * refusals is what turns "the compiler took part in 14 programs" into a list
   * of the next things to build, in the order they would pay. */
  printf("\n  %d of %d lowered to typed IR\n", lowered, hotCount);

  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir != NULL || hot[i].irRefusal == NULL) continue;
    int same = 0;
    for (int j = 0; j < i; j++) {
      if (hot[j].irRefusal != NULL && hot[j].ir == NULL && strcmp(hot[j].irRefusal, hot[i].irRefusal) == 0) {
        same = 1;
        break;
      }
    }
    if (same) continue;

    int count = 0;
    for (int j = 0; j < hotCount; j++) {
      if (hot[j].ir == NULL && hot[j].irRefusal != NULL && strcmp(hot[j].irRefusal, hot[i].irRefusal) == 0) {
        count++;
      }
    }
    printf("    %3d not lowered: %s\n", count, hot[i].irRefusal);
  }

  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL) continue;
    const char *producer = NULL;
    const char *consumer = NULL;
    if (!csIrFirstUntyped(hot[i].ir, &producer, &consumer)) continue;
    const char *name = hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>";
    printf("    %-24.24s untyped: %s wanted a number from %s\n", name, consumer, producer);
  }
  if (totalRegisters > 0) {
    /* The number stage 2 exists to produce: how much of a hot function's
     * data flow is provably numeric, and could therefore live unboxed in a
     * register with no guard on it. */
    printf("  %d of %d IR values (%.0f%%) are known to be numbers\n", typedRegisters, totalRegisters, 100.0 * (double)typedRegisters / (double)totalRegisters);
  }
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL && hot[i].irRefusal != NULL) {
      printf("  %-24s did not lower: %s\n", hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>", hot[i].irRefusal);
    }
  }
  if (getenv("CS_JIT_DUMP_IR") != NULL) {
    printf("\n");
    for (int i = 0; i < hotCount; i++) {
      if (hot[i].ir != NULL) csIrPrint(hot[i].ir);
    }
  }

  int compiled = 0;
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].code != NULL) compiled++;
  }
  printf("  %d of %d compiled to machine code\n", compiled, hotCount);
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].code != NULL) continue;
    const char *name = hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>";
    if (hot[i].ir == NULL) continue; /* the lowering already said why */
    const char *why = hot[i].codeRefusal;
    if (why == NULL) why = "not fully typed";
    printf("    %-24.24s not compiled: %s\n", name, why);
  }
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir != NULL && hot[i].code == NULL && hot[i].codeRefusal != NULL) {
      printf("  %-24s did not compile: %s\n", hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>", hot[i].codeRefusal);
    }
  }
  int inlinedCalls = 0;
  int inlinedInstructions = 0;
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL) continue;
    inlinedCalls += hot[i].ir->inlinedCount;
    inlinedInstructions += hot[i].ir->inlinedInstructions;
  }
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL || hot[i].ir->firstReplayRefusal == NULL) continue;
    printf("    %-24.24s lost the blocks past a hand-over: could not replay %s\n", hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>",
           hot[i].ir->firstReplayRefusal);
  }
  if (inlinedCalls > 0) {
    printf("  %d callee%s spliced in, %d instruction%s of body\n", inlinedCalls, inlinedCalls == 1 ? "" : "s", inlinedInstructions,
           inlinedInstructions == 1 ? "" : "s");
  }
  printf("  %ld call%s answered without the interpreter\n", substituted, substituted == 1 ? "" : "s");
  printf("  %ld loop%s taken over while already running\n", osrEntered, osrEntered == 1 ? "" : "s");
  printf("  %ld handed back to the interpreter part-way\n", exited);
  printf("  %ld of those deoptimised into a frame built on the way out\n", deoptimised);

  int sites = typedTotal + genericTotal;
  printf("\n  %d of %d compilable without falling back to the interpreter\n", compilable, hotCount);
  if (sites > 0) {
    /* The number this stage exists to produce. Every typed site is an
     * operation a compiler could emit unboxed, with no guard and no
     * deoptimisation point — which is the whole argument for a type-directed
     * backend over a speculative one. */
    /* Arithmetic only: those are the sites the compiler currently consults a
     * resolved type for. A wider count would need the checker to record more
     * than it does, which is itself a finding. */
    printf("  %d of %d arithmetic sites (%.0f%%) have both operand types known\n", typedTotal, sites, 100.0 * (double)typedTotal / (double)sites);
  }
  printf("\n");
}
