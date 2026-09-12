/* jitcode.c — compiling a lowered function, and the memory it runs from.
 *
 * Only the fully-typed numeric functions the IR can prove are compiled: every
 * operand of every arithmetic operation is known to be a number, so the code
 * has no type checks, no guards and nothing to deoptimise to. That is the
 * whole reason this design starts here rather than with a speculative
 * compiler — a JavaScript engine cannot know these things and CScript can.
 *
 * NaN-boxing is what makes the load path free. A number's Value *is* its
 * double, bit for bit, so a slot holding one is loaded straight into a
 * floating-point register: no tag test, no unboxing, no conversion. Under the
 * tagged union that is simply false, so the `tagged` build gets no backend
 * rather than a broken one.
 *
 * This file decides what goes where and lays the blocks out. The arm64
 * instructions are in jitcode_arm64.c, the decisions about registers in
 * jitcode_alloc.c, and what each IR instruction becomes in jitcode_emit.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cscript/jitcode.h"
#include "cscript/object.h"

#include "jit/jitcode_internal.h"

#if defined(__APPLE__) && defined(__arm64__) && CS_NAN_BOXING
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#define CS_JIT_APPLE_SILICON 1
#endif

/* ---- executable memory -------------------------------------------------- */

static JitCode *publish(Encoder *encoder, int scratchCount) {
  size_t size = (size_t)encoder->count * sizeof(uint32_t);
  if (size == 0) return NULL;

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef CS_JIT_APPLE_SILICON
  /* Apple Silicon enforces write-xor-execute per thread: a page may be mapped
   * both ways, but a thread may only do one at a time, and the switch is
   * explicit. Without MAP_JIT the mapping is refused outright. */
  flags |= MAP_JIT;
#endif

  void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
  if (memory == MAP_FAILED) return NULL;

#ifdef CS_JIT_APPLE_SILICON
  pthread_jit_write_protect_np(0); /* this thread may now write */
#endif
  memcpy(memory, encoder->words, size);
#ifdef CS_JIT_APPLE_SILICON
  pthread_jit_write_protect_np(1); /* and may now execute instead */
  /* The instruction cache does not see the stores otherwise, and the code that
   * runs is whatever happened to be in the page before. */
  sys_icache_invalidate(memory, size);
#endif

  JitCode *code = (JitCode *)calloc(1, sizeof(JitCode));
  code->memory = memory;
  code->size = size;
  code->entry = (CompiledFn)memory;
  code->scratchCount = scratchCount;
  return code;
}

void csJitCodeFree(JitCode *code) {
  if (code == NULL) return;
  munmap(code->memory, code->size);
  free(code->exits);
  free(code);
}

JitCode *csJitCompile(const IrFunction *ir, const char **why) {
  *why = NULL;
#ifndef CS_JIT_APPLE_SILICON
  *why = "no code generator for this architecture and Value layout";
  return NULL;
#else
  Encoder encoder;
  memset(&encoder, 0, sizeof encoder);

  /* Promoted slots claim registers first; the per-block allocator then works
   * with whatever is left. There are 22 to start with and a function this
   * backend accepts has a handful of slots, so the two do not compete. */
  /* Only modulo calls out today — arm64 has no floating-point remainder — but
   * the choice is per function rather than per instruction, because a register
   * has to be safe for the whole of the run it is live in. */
  bool callSafe = false;
  for (int b = 0; b < ir->blockCount && !callSafe; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      if (ir->blocks[b].instructions[i].op == IR_MOD) {
        callSafe = true;
        break;
      }
    }
  }
  int pool = csJitPoolSize(callSafe);

  bool *promotable = csJitPromotableSlots(ir);
  int *slotHome = (int *)malloc(sizeof(int) * (size_t)(ir->slotCount + 1));
  int promotedCount = 0;
  for (int s = 0; s <= ir->slotCount; s++) {
    slotHome[s] = -1;
    if (promotable[s] && promotedCount < pool / 2) {
      slotHome[s] = csJitAllocPool(pool - 1 - promotedCount, callSafe);
      promotedCount++;
    }
  }
  free(promotable);
  if (getenv("CS_JIT_DUMP_IR") != NULL) {
    printf("  compiling %s: %d of %d slots promoted to registers\n", ir->source->name != NULL ? ir->source->name->chars : "<top level>", promotedCount,
           ir->slotCount + 1);
  }

  /* Constants get registers too, materialised once on entry.
   *
   * A 64-bit immediate takes up to four movz/movk and an fmov to reach a
   * floating-point register. Inside a loop that is paid every iteration for a
   * value that by definition never changes — and in the loop benchmark it was
   * three constants and about fifteen instructions of the body. */
  uint64_t constantValue[ALLOC_POOL_SIZE];
  int constantHome[ALLOC_POOL_SIZE];
  int constantCount = 0;
  for (int b = 0; b < ir->blockCount && constantCount < pool / 2; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op != IR_CONST) continue;
      uint64_t bits;
      memcpy(&bits, &inst->constant, sizeof bits);

      bool seen = false;
      for (int k = 0; k < constantCount; k++) {
        if (constantValue[k] == bits) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (promotedCount + constantCount >= pool / 2) break;
      constantValue[constantCount] = bits;
      constantHome[constantCount] = csJitAllocPool(pool - 1 - promotedCount - constantCount, callSafe);
      constantCount++;
    }
  }

  /* One address register per distinct global. Resolved here, while the
   * program is running and the binding certainly exists — the function only
   * reached this point by being hot, which is after its module ran. */
  int globalName[CS_JIT_MAX_GLOBALS];
  Value *globalAddress[CS_JIT_MAX_GLOBALS];
  int globalCount = 0;
  Table *globalTable = ir->source->module != NULL ? &ir->source->module->globals : NULL;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op != IR_LOAD_GLOBAL && inst->op != IR_STORE_GLOBAL) continue;

      bool seen = false;
      for (int g = 0; g < globalCount; g++) {
        if (globalName[g] == inst->a) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (globalCount >= CS_JIT_MAX_GLOBALS || globalTable == NULL) {
        *why = "more globals than there are registers to hold them";
        return NULL;
      }

      Value name = ir->source->chunk.constants.values[inst->a];
      Entry *entry = IS_STRING(name) ? csTableFindEntry(globalTable, AS_STRING(name)) : NULL;
      if (entry == NULL || entry->key == NULL) {
        *why = "a global that is not there yet";
        return NULL;
      }
      globalName[globalCount] = inst->a;
      globalAddress[globalCount] = &entry->value;
      globalCount++;
    }
  }

  JitExit *exits = NULL;
  int exitCount = 0, exitCapacity = 0;

  int *home = csJitAllocateRegisters(ir, promotedCount + constantCount, callSafe);
  int *blockStart = (int *)malloc(sizeof(int) * (size_t)ir->blockCount);

  csJitEmitPrologue(&encoder);

  /* Load the promoted slots and materialise the constants, once, on entry. */
  for (int s = 0; s <= ir->slotCount; s++) {
    if (slotHome[s] >= 0) csJitLdrDouble(&encoder, slotHome[s], REG_SLOTS, s * 8);
  }
  for (int k = 0; k < constantCount; k++) {
    csJitMovImmediate(&encoder, REG_TEMP, constantValue[k]);
    csJitFmovToDouble(&encoder, constantHome[k], REG_TEMP);
  }
  for (int g = 0; g < globalCount; g++) {
    csJitMovImmediate(&encoder, REG_FIRST_GLOBAL + g, (uint64_t)(uintptr_t)globalAddress[g]);
  }
  Fixup *fixups = NULL;
  int fixupCount = 0, fixupCapacity = 0;

  for (int b = 0; b < ir->blockCount; b++) {
    blockStart[b] = encoder.count;

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      EmitAt at = {&encoder,   ir,          inst,   home,       slotHome,      constantValue, constantHome, constantCount,
                   globalName, globalCount, &exits, &exitCount, &exitCapacity, &fixups,       &fixupCount,  &fixupCapacity,
                   b,          &i,          why};
      if (!csJitEmitInstruction(&at)) goto unsupported;
    }
  }

  csJitMovImmediate(&encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
  csJitWord(&encoder, 0xAA0903E0u); /* mov x0, x9 */
  csJitEmitEpilogue(&encoder);

  /* One extra entry per loop header, so a running loop can be taken over.
   *
   * Each is the function prologue again — load the promoted slots, materialise
   * the constants — and then a jump into the header. The prologue is what
   * makes the hand-over correct: promotion means a slot's live value is in a
   * register, and at this point the live value is the interpreter's, in the
   * frame. Reading them here is the whole of the state transfer. */
  int osrBlock[CS_JIT_MAX_OSR];
  int osrWord[CS_JIT_MAX_OSR];
  int osrCount = 0;
  if (csJitBlocksAreSelfContained(ir)) {
    for (int b = 0; b < ir->blockCount && osrCount < CS_JIT_MAX_OSR; b++) {
      if (!csJitIsLoopHeader(ir, b)) continue;

      osrBlock[osrCount] = b;
      osrWord[osrCount] = encoder.count;
      osrCount++;

      csJitEmitPrologue(&encoder);
      for (int s = 0; s <= ir->slotCount; s++) {
        if (slotHome[s] >= 0) csJitLdrDouble(&encoder, slotHome[s], REG_SLOTS, s * 8);
      }
      for (int k = 0; k < constantCount; k++) {
        csJitMovImmediate(&encoder, REG_TEMP, constantValue[k]);
        csJitFmovToDouble(&encoder, constantHome[k], REG_TEMP);
      }
      for (int g = 0; g < globalCount; g++) {
        csJitMovImmediate(&encoder, REG_FIRST_GLOBAL + g, (uint64_t)(uintptr_t)globalAddress[g]);
      }

      if (fixupCapacity < fixupCount + 1) {
        fixupCapacity = fixupCapacity < 8 ? 8 : fixupCapacity * 2;
        fixups = (Fixup *)realloc(fixups, sizeof(Fixup) * (size_t)fixupCapacity);
      }
      fixups[fixupCount++] = (Fixup){encoder.count, b, false, 0};
      csJitWord(&encoder, 0x14000000u);
    }
  }

  if (encoder.failed) {
    *why = "an offset too large to encode";
    goto unsupported;
  }

  for (int f = 0; f < fixupCount; f++) {
    int target = blockStart[fixups[f].block];
    int delta = target - fixups[f].at;
    if (fixups[f].conditional) {
      if (delta < -(1 << 18) || delta >= (1 << 18)) {
        *why = "branch out of range";
        goto unsupported;
      }
      encoder.words[fixups[f].at] = 0x54000000u | (((uint32_t)delta & 0x7ffffu) << 5) | fixups[f].condition;
    } else {
      if (delta < -(1 << 25) || delta >= (1 << 25)) {
        *why = "branch out of range";
        goto unsupported;
      }
      encoder.words[fixups[f].at] = 0x14000000u | ((uint32_t)delta & 0x3ffffffu);
    }
  }

  JitCode *code = publish(&encoder, ir->registerCount);
  if (code != NULL) {
    code->exits = exits;
    code->exitCount = exitCount;
    code->globalTable = globalTable;
    code->globalVersion = globalTable != NULL ? globalTable->version : 0;
    for (int g = 0; g < globalCount; g++) code->globalAddress[g] = globalAddress[g];
    code->globalCount = globalCount;
    for (int o = 0; o < osrCount; o++) {
      code->osr[o].bytecodeOffset = ir->blocks[osrBlock[o]].bytecodeStart;
      code->osr[o].entry = (CompiledFn)((uint32_t *)code->memory + osrWord[o]);
    }
    code->osrCount = osrCount;
  }
  free(slotHome);
  free(home);
  free(blockStart);
  free(fixups);
  free(encoder.words);
  if (code == NULL) *why = "could not map executable memory";
  return code;

unsupported:
  free(exits);
  free(slotHome);
  free(home);
  free(blockStart);
  free(fixups);
  free(encoder.words);
  return NULL;
#endif
}
