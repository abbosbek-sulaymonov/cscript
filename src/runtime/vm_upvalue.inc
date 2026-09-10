/* vm_upvalue.inc — captured locals, and the opcode profile.
 *
 * An upvalue is open while the local is still on the stack and closed when the
 * scope leaves, so two closures over one variable share it either way — which
 * is what makes a captured counter behave as JavaScript says it does.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
/* Finds or creates the upvalue for a stack slot.
 *
 * The open list is kept sorted by descending slot address, so two closures
 * capturing the same variable share one upvalue — which is what makes writes
 * through one visible to the other. */
static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *previous = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;

  while (upvalue != NULL && upvalue->location > local) {
    previous = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) return upvalue;

  ObjUpvalue *created = csUpvalueNew(local);
  created->next = upvalue;
  if (previous == NULL) {
    vm.openUpvalues = created;
  } else {
    previous->next = created;
  }
  return created;
}

/* Moves every upvalue at or above `last` off the stack and onto its own heap
 * cell, so closures keep working once the frame is gone. */
static void closeUpvalues(Value *last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

#ifdef CS_DEBUG_PROFILE_OPCODES
/* Counts how often each opcode follows each other opcode, so superinstruction
 * candidates are chosen from data rather than from intuition. */
static uint64_t opcodePairs[OP_COUNT][OP_COUNT];
static uint64_t opcodeCounts[OP_COUNT];
static int previousOpcode = -1;

static void recordOpcode(uint8_t opcode) {
  opcodeCounts[opcode]++;
  if (previousOpcode >= 0) opcodePairs[previousOpcode][opcode]++;
  previousOpcode = opcode;
}

void csVMDumpOpcodeProfile(void);

void csVMDumpOpcodeProfile(void) {
  uint64_t total = 0;
  for (int i = 0; i < OP_COUNT; i++) total += opcodeCounts[i];
  if (total == 0) return;

  fprintf(stderr, "\n== opcode profile: %llu instructions ==\n",
          (unsigned long long)total);

  /* Top single opcodes. */
  fprintf(stderr, "\n-- most executed --\n");
  for (int rank = 0; rank < 10; rank++) {
    int best = -1;
    for (int i = 0; i < OP_COUNT; i++) {
      if (opcodeCounts[i] > 0 && (best < 0 || opcodeCounts[i] > opcodeCounts[best])) {
        best = i;
      }
    }
    if (best < 0) break;
    fprintf(stderr, "  %-22s %10llu  %5.1f%%\n", csOpcodeName((OpCode)best),
            (unsigned long long)opcodeCounts[best],
            100.0 * (double)opcodeCounts[best] / (double)total);
    opcodeCounts[best] = 0;
  }

  /* Top adjacent pairs — each one is a superinstruction candidate. */
  fprintf(stderr, "\n-- most frequent pairs --\n");
  for (int rank = 0; rank < 15; rank++) {
    int bestA = -1, bestB = -1;
    uint64_t best = 0;
    for (int a = 0; a < OP_COUNT; a++) {
      for (int b = 0; b < OP_COUNT; b++) {
        if (opcodePairs[a][b] > best) {
          best = opcodePairs[a][b];
          bestA = a;
          bestB = b;
        }
      }
    }
    if (bestA < 0) break;
    fprintf(stderr, "  %-22s -> %-22s %9llu  %5.1f%%\n", csOpcodeName((OpCode)bestA),
            csOpcodeName((OpCode)bestB), (unsigned long long)best,
            100.0 * (double)best / (double)total);
    opcodePairs[bestA][bestB] = 0;
  }
  fprintf(stderr, "\n");
}
#endif

/* Dispatch strategy.
 *
 * A switch compiles to one indirect branch shared by every opcode, so the CPU's
 * branch predictor has a single history slot for the whole interpreter and
 * mispredicts constantly. Computed goto gives each opcode its own dispatch
 * branch at the end of its own handler, so the predictor can learn the pairs
 * that actually follow one another — OP_GET_LOCAL is nearly always followed by
 * OP_CONSTANT in a loop, and that becomes predictable.
 *
 * The switch version is kept for compilers without the labels-as-values
 * extension, and both paths run the same handler bodies.
 *
 * Define CS_NO_COMPUTED_GOTO to force the portable switch path — `make
 * test-switch` builds that way, so the fallback cannot quietly rot. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(CS_NO_COMPUTED_GOTO)
#define CS_COMPUTED_GOTO 1
#endif

/* Labels-as-values is a GNU extension, which -Wpedantic flags. The use is
 * deliberate and guarded by the feature test above, so the warning is silenced
 * here rather than dropped from the project-wide warning set. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

