/* jit.c — tiering, with no code generator behind it yet.
 *
 * See jit.h for why this exists before the backend does. In short: the
 * decision of *what* to compile has to be shown to be right before the
 * machinery to compile it is worth writing, and the interesting part of that
 * decision here is how much of a hot function's work is already typed.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/debug.h"
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
  const char *refusal; /* NULL when the function could be compiled */
} HotEntry;

static HotEntry hot[CS_JIT_MAX_HOT];
static int hotCount = 0;

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

void csJitConsider(ObjFunction *function) {
  if (function->jitState != JIT_INTERPRETED) return;

  const char *refusal = scanChunk(&function->chunk);
  function->jitState = refusal == NULL ? JIT_HOT : JIT_REFUSED;

  if (hotCount < CS_JIT_MAX_HOT) {
    hot[hotCount].function = function;
    hot[hotCount].refusal = refusal;
    hotCount++;
  }
}

const char *csJitRefusalReason(const ObjFunction *function) {
  for (int i = 0; i < hotCount; i++) {
    if (hot[i].function == function) return hot[i].refusal;
  }
  return NULL;
}

void csJitDumpProfile(void) {
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
