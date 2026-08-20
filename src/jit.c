/* jit.c — tiering, with no code generator behind it yet.
 *
 * See jit.h for why this exists before the backend does. In short: the
 * decision of *what* to compile has to be shown to be right before the
 * machinery to compile it is worth writing, and the interesting part of that
 * decision here is how much of a hot function's work is already typed.
 */
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
  const char *refusal;      /* why a backend would skip it, or NULL */
  const char *irRefusal;    /* why it would not lower, or NULL */
  IrFunction *ir;           /* the lowered form, when it lowered */
  JitCode *code;            /* machine code, when it compiled */
  const char *codeRefusal;
  Value *scratch;           /* the compiled code's working space */
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

/* Opcodes a first backend would not attempt.
 *
 * Not a permanent list — it is what the *first* code generator would leave to
 * the interpreter, either because it suspends a frame, because it can throw
 * past one, or because it calls back into the VM in a way a compiled frame
 * would have to model. Everything else is arithmetic, moves and branches. */
static const char *unsupportedOpcode(uint8_t opcode) {
  switch (opcode) {
    case OP_AWAIT:
      return "suspends the frame (await)";
    case OP_TRY:
    case OP_THROW:
    case OP_END_TRY:
      return "unwinds past the frame (try/throw)";
    case OP_NEW:
    case OP_SUPER_CALL:
    case OP_SUPER_INVOKE:
    case OP_GET_SUPER:
      return "constructs or calls through a class";
    case OP_IMPORT_NAME:
    case OP_IMPORT_NAMESPACE:
      return "resolves a module";
    case OP_CLASS:
    case OP_INHERIT:
    case OP_METHOD:
    case OP_STATIC_METHOD:
    case OP_STATIC_FIELD:
    case OP_CONSTRUCTOR:
    case OP_FIELD_INIT:
    case OP_GETTER:
    case OP_SETTER:
      return "builds a class";
    default:
      return NULL;
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

int csJitThreshold(void) {
  static int threshold = -1;
  if (threshold < 0) {
    const char *override = getenv("CS_JIT_THRESHOLD");
    threshold = override != NULL ? atoi(override) : 10000;
    if (threshold < 1) threshold = 1;
  }
  return threshold;
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

bool csJitTryRun(ObjFunction *function, const Value *args, int argCount, Value *out) {
  /* Both states are runnable: JIT_HOT has lowered IR, JIT_COMPILED also has
   * machine code. Admitting only the first rejected exactly the functions that
   * had got furthest. */
  if (function->jitState != JIT_HOT && function->jitState != JIT_COMPILED) return false;
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].function != function) continue;
    if (hot[i].ir == NULL) return false;
    /* Only where every value is proved: see csIrIsFullyTyped. */
    if (!csIrIsFullyTyped(hot[i].ir)) return false;
    /* A body with an exit in it is entered only through OSR, where a frame
     * already exists for the interpreter to be handed back. */
    if (hot[i].ir->hasExits) return false;

    /* Compiled code, when there is any. The frame it needs is the slots array
     * the interpreter would have used, with the arguments already in place. */
    if (hot[i].code != NULL) {
      Value slots[256];
      if (hot[i].ir->slotCount > 256) return false;
      for (int s = 0; s < hot[i].ir->slotCount; s++) slots[s] = UNDEFINED_VAL;
      for (int a = 0; a < argCount && a + 1 < hot[i].ir->slotCount; a++) {
        slots[a + 1] = args[a];
      }
      /* A call entry cannot take an exit: there is no frame to hand back, and
       * csIrLower only produces one for code the interpreter would resume. */
      if (!assumptionsHold(hot[i].code)) return false;

      int exit = -1;
      uint64_t bits = hot[i].code->entry(slots, hot[i].scratch, &exit);
      if (exit >= 0) return false;
      memcpy(out, &bits, sizeof(Value));
      substituted++;
      return true;
    }
    if (!csIrInterpret(hot[i].ir, args, argCount, out)) return false;
    substituted++;
    return true;
  }
  return false;
}

bool csJitOsr(ObjFunction *function, int bytecodeOffset, Value *slots,
              Value *out, int *resumeAt, int *resumeHeight) {
  *resumeAt = -1;
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].function != function) continue;
    if (hot[i].code == NULL) return false;

    for (int o = 0; o < hot[i].code->osrCount; o++) {
      if (hot[i].code->osr[o].bytecodeOffset != bytecodeOffset) continue;

      /* The compiled code writes every slot the IR knows about, including any
       * belonging to a scope the interpreter has not opened yet. Those sit
       * above the frame's current top, so the room has to be there. */
      if (slots + hot[i].ir->slotCount >= vm.stack + vm.stackCapacity) return false;
      if (!assumptionsHold(hot[i].code)) return false;

      int exit = -1;
      uint64_t bits = hot[i].code->osr[o].entry(slots, hot[i].scratch, &exit);

      if (exit >= 0 && exit < hot[i].code->exitCount) {
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

  /* Machine code only where every arithmetic operand is proved a number.
   * Nothing else is attempted, because anything else would need a guard. */
  if (hot[hotCount].ir != NULL && csIrIsFullyTyped(hot[hotCount].ir)) {
    const char *codeWhy = NULL;
    hot[hotCount].code = csJitCompile(hot[hotCount].ir, &codeWhy);
    hot[hotCount].codeRefusal = codeWhy;
    if (hot[hotCount].code != NULL) {
      hot[hotCount].scratch =
          (Value *)calloc((size_t)hot[hotCount].ir->registerCount + 1, sizeof(Value));
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
  if (getenv("CS_JIT_REPORT") == NULL) return;

  if (hotCount == 0) {
    printf("\n== tiering: nothing reached %d ==\n", CS_JIT_THRESHOLD);
    return;
  }

  printf("\n== tiering: %d function%s over %d ==\n\n", hotCount,
         hotCount == 1 ? "" : "s", CS_JIT_THRESHOLD);
  printf("  %-28s %10s %8s %8s  %s\n", "function", "hotness", "typed", "generic",
         "verdict");

  int compilable = 0;
  int typedTotal = 0;
  int genericTotal = 0;

  for (int i = 0; i < hotCount; i++) {
    ObjFunction *function = hot[i].function;
    const char *name =
        function->name != NULL ? function->name->chars : "<top level>";

    typedTotal += function->typedSites;
    genericTotal += function->genericSites;
    if (hot[i].refusal == NULL) compilable++;

    printf("  %-28.28s %10d %8d %8d  %s\n", name, function->hotness,
           function->typedSites, function->genericSites,
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

  printf("\n  %d of %d lowered to typed IR\n", lowered, hotCount);
  if (totalRegisters > 0) {
    /* The number stage 2 exists to produce: how much of a hot function's
     * data flow is provably numeric, and could therefore live unboxed in a
     * register with no guard on it. */
    printf("  %d of %d IR values (%.0f%%) are known to be numbers\n", typedRegisters,
           totalRegisters, 100.0 * (double)typedRegisters / (double)totalRegisters);
  }
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].ir == NULL && hot[i].irRefusal != NULL) {
      printf("  %-24s did not lower: %s\n",
             hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>",
             hot[i].irRefusal);
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
    if (hot[i].ir != NULL && hot[i].code == NULL && hot[i].codeRefusal != NULL) {
      printf("  %-24s did not compile: %s\n",
             hot[i].function->name != NULL ? hot[i].function->name->chars : "<top level>",
             hot[i].codeRefusal);
    }
  }
  printf("  %ld call%s answered without the interpreter\n", substituted,
         substituted == 1 ? "" : "s");
  printf("  %ld loop%s taken over while already running\n", osrEntered,
         osrEntered == 1 ? "" : "s");
  printf("  %ld handed back to the interpreter part-way\n", exited);

  int sites = typedTotal + genericTotal;
  printf("\n  %d of %d compilable without falling back to the interpreter\n",
         compilable, hotCount);
  if (sites > 0) {
    /* The number this stage exists to produce. Every typed site is an
     * operation a compiler could emit unboxed, with no guard and no
     * deoptimisation point — which is the whole argument for a type-directed
     * backend over a speculative one. */
    /* Arithmetic only: those are the sites the compiler currently consults a
     * resolved type for. A wider count would need the checker to record more
     * than it does, which is itself a finding. */
    printf("  %d of %d arithmetic sites (%.0f%%) have both operand types known\n",
           typedTotal, sites, 100.0 * (double)typedTotal / (double)sites);
  }
  printf("\n");
}
