/* jitcode.c — an arm64 encoder, and the executable memory it writes into.
 *
 * Deliberately a straightforward translation rather than a good one. Every IR
 * value gets a home in a scratch array and every instruction loads its
 * operands and stores its result, which is what a register allocator would
 * later remove. Getting the pipeline correct end to end is worth more than
 * getting the code good: the allocator is a separate change with its own
 * measurement, and it cannot be written before there is something to allocate
 * for.
 *
 * What this does remove is everything the interpreter was doing *around* the
 * arithmetic: no dispatch, no operand-stack traffic, no tag tests, no boxing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cscript/jitcode.h"

#if defined(__APPLE__) && defined(__arm64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#define CS_JIT_APPLE_SILICON 1
#endif

/* ---- the buffer -------------------------------------------------------- */

typedef struct {
  uint32_t *words;
  int count;
  int capacity;
  bool failed;
} Encoder;

static void word(Encoder *encoder, uint32_t instruction) {
  if (encoder->capacity < encoder->count + 1) {
    encoder->capacity = encoder->capacity < 64 ? 64 : encoder->capacity * 2;
    encoder->words =
        (uint32_t *)realloc(encoder->words, sizeof(uint32_t) * (size_t)encoder->capacity);
  }
  encoder->words[encoder->count++] = instruction;
}

/* ---- arm64 encodings ---------------------------------------------------
 *
 * Only the handful this needs. Each is written out rather than built by a
 * general assembler, because a general assembler is a project of its own and
 * eight instructions are not.
 */

/* LDR <Dt>, [<Xn>, #imm]  — imm is a byte offset, and must be a multiple of 8 */
static void ldrDouble(Encoder *encoder, int destination, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    encoder->failed = true;
    return;
  }
  word(encoder, 0xFD400000u | ((uint32_t)(byteOffset / 8) << 10) |
                    ((uint32_t)base << 5) | (uint32_t)destination);
}

/* STR <Dt>, [<Xn>, #imm] */
static void strDouble(Encoder *encoder, int source, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    encoder->failed = true;
    return;
  }
  word(encoder, 0xFD000000u | ((uint32_t)(byteOffset / 8) << 10) |
                    ((uint32_t)base << 5) | (uint32_t)source);
}

static void fadd(Encoder *e, int d, int n, int m) {
  word(e, 0x1E602800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fsub(Encoder *e, int d, int n, int m) {
  word(e, 0x1E603800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fmul(Encoder *e, int d, int n, int m) {
  word(e, 0x1E600800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fdiv(Encoder *e, int d, int n, int m) {
  word(e, 0x1E601800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fneg(Encoder *e, int d, int n) {
  word(e, 0x1E614000u | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fcmp(Encoder *e, int n, int m) {
  word(e, 0x1E602000u | ((uint32_t)m << 16) | ((uint32_t)n << 5));
}

/* MOVZ/MOVK <Xd>, #imm16, LSL #shift — a 64-bit constant in four pieces. */
static void movImmediate(Encoder *e, int destination, uint64_t value) {
  word(e, 0xD2800000u | ((uint32_t)(value & 0xffff) << 5) | (uint32_t)destination);
  for (int part = 1; part < 4; part++) {
    uint32_t piece = (uint32_t)((value >> (16 * part)) & 0xffff);
    if (piece == 0) continue;
    word(e, 0xF2800000u | ((uint32_t)part << 21) | (piece << 5) | (uint32_t)destination);
  }
}

/* FMOV <Dd>, <Xn> and FMOV <Xd>, <Dn> — moving bits between the register
 * files, which is all that is needed because a number's Value is its double. */
static void fmovToDouble(Encoder *e, int d, int n) {
  word(e, 0x9E670000u | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fmovToGeneral(Encoder *e, int d, int n) {
  word(e, 0x9E660000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* LDR <Xt>, [<Xn>, #imm] — the general-register form, for booleans, which are
 * NaN-boxed singletons rather than doubles. */
static void ldrGeneral(Encoder *e, int destination, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    e->failed = true;
    return;
  }
  word(e, 0xF9400000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) |
              (uint32_t)destination);
}

static void strGeneral(Encoder *e, int source, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    e->failed = true;
    return;
  }
  word(e, 0xF9000000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) |
              (uint32_t)source);
}

/* CMP <Xn>, <Xm> */
static void cmpGeneral(Encoder *e, int n, int m) {
  word(e, 0xEB00001Fu | ((uint32_t)m << 16) | ((uint32_t)n << 5));
}

/* CSEL <Xd>, <Xn>, <Xm>, cond — d = cond ? n : m */
static void csel(Encoder *e, int d, int n, int m, uint32_t condition) {
  word(e, 0x9A800000u | ((uint32_t)m << 16) | (condition << 12) | ((uint32_t)n << 5) |
              (uint32_t)d);
}

static void ret(Encoder *e) { word(e, 0xD65F03C0u); }

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
  free(code);
}

/* ---- IR to machine code ------------------------------------------------- */

/* x0 holds the frame slots, x1 the scratch array for IR values. d0 and d1 are
 * the working registers; d2 holds a materialised constant. Nothing is kept in
 * a register across an instruction, which is the simplification a register
 * allocator would later undo. */
#define REG_SLOTS 0
#define REG_SCRATCH 1
#define REG_TEMP 9

/* A branch whose target block is not laid out yet. */
typedef struct {
  int at;        /* index of the instruction to patch */
  int block;     /* where it should go */
  bool conditional;
  uint32_t condition;
} Fixup;

/* arm64 condition codes for the comparisons, on the *ordered* forms — an
 * unordered compare (either side NaN) must not take the branch, which is what
 * distinguishes MI/GT/LS from LT/GE and matters for `x < NaN`. */
#define COND_MI 0x4u /* less than, ordered */
#define COND_LS 0x9u /* less or equal, ordered */
#define COND_GT 0xcu
#define COND_GE 0xau
#define COND_EQ 0x0u
#define COND_NE 0x1u

static uint32_t conditionFor(IrOp op) {
  switch (op) {
    case IR_LT: return COND_MI;
    case IR_LE: return COND_LS;
    case IR_GT: return COND_GT;
    case IR_GE: return COND_GE;
    case IR_EQ: return COND_EQ;
    default: return COND_NE;
  }
}


/* ---- register allocation ------------------------------------------------
 *
 * Without this the compiled code was *slower* than the interpreter: every IR
 * value went to memory and came back, so a two-multiply function did more
 * loads than the bytecode did. Removing dispatch bought less than that cost.
 *
 * A linear scan per block, which is enough because the IR is nearly all
 * block-local once redundant slot round-trips have been forwarded. A value
 * used outside the block that defined it keeps a memory home — proving when
 * that is safe needs liveness across the whole graph, and the values it would
 * catch are rare.
 *
 * d0 and d1 stay free as scratch for operands that did not get a register.
 * d8–d15 are avoided because they are callee-saved and using them would mean
 * a prologue; d2–d7 and d16–d31 are free for the taking, which is 22 and more
 * than any block here needs.
 */
#define ALLOC_FIRST_SCRATCH 0
#define ALLOC_SECOND_SCRATCH 1
#define ALLOC_POOL_SIZE 22

static int allocPool(int index) {
  /* d2..d7, then d16..d31 — skipping the callee-saved bank entirely. */
  return index < 6 ? 2 + index : 16 + (index - 6);
}

/* -1 means the value lives in the scratch array rather than a register. */
static int *allocateRegisters(const IrFunction *ir) {
  int *assignment = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  int *definedIn = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  bool *escapes = (bool *)calloc((size_t)ir->registerCount + 1, sizeof(bool));

  for (int r = 0; r <= ir->registerCount; r++) {
    assignment[r] = -1;
    definedIn[r] = -1;
  }

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->result >= 0) definedIn[inst->result] = b;
    }
  }

  /* A value read in a block other than the one that defined it — or read
   * before it, which a loop back-edge makes possible — stays in memory. */
  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      int operands[2] = {inst->a, inst->b};
      /* For these, `a` is a slot number rather than a value. */
      if (inst->op == IR_LOAD_LOCAL || inst->op == IR_JUMP) operands[0] = -1;
      if (inst->op == IR_STORE_LOCAL) operands[0] = -1;
      if (inst->op == IR_BRANCH || inst->op == IR_RETURN || inst->op == IR_NEG ||
          inst->op == IR_CONST || inst->op == IR_LOAD_LOCAL) {
        operands[1] = -1;
      }
      for (int k = 0; k < 2; k++) {
        int value = operands[k];
        if (value < 0 || value >= ir->registerCount) continue;
        if (definedIn[value] != b) escapes[value] = true;
      }
    }
  }

  /* Last use within the defining block, so a register can be handed back. */
  int *lastUse = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));
  for (int r = 0; r <= ir->registerCount; r++) lastUse[r] = -1;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int r = 0; r <= ir->registerCount; r++) lastUse[r] = -1;
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->a >= 0 && inst->a < ir->registerCount && inst->op != IR_LOAD_LOCAL &&
          inst->op != IR_STORE_LOCAL && inst->op != IR_JUMP) {
        lastUse[inst->a] = i;
      }
      if (inst->op == IR_STORE_LOCAL && inst->b >= 0 && inst->b < ir->registerCount) {
        lastUse[inst->b] = i;
      }
      if (inst->b >= 0 && inst->b < ir->registerCount && inst->op != IR_STORE_LOCAL &&
          inst->op != IR_BRANCH && inst->op != IR_RETURN && inst->op != IR_NEG &&
          inst->op != IR_CONST && inst->op != IR_LOAD_LOCAL && inst->op != IR_JUMP) {
        lastUse[inst->b] = i;
      }
    }

    bool taken[ALLOC_POOL_SIZE];
    int holder[ALLOC_POOL_SIZE];
    for (int k = 0; k < ALLOC_POOL_SIZE; k++) { taken[k] = false; holder[k] = -1; }

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      /* Free anything whose last use was the previous instruction. */
      for (int k = 0; k < ALLOC_POOL_SIZE; k++) {
        if (taken[k] && holder[k] >= 0 && lastUse[holder[k]] >= 0 &&
            lastUse[holder[k]] < i) {
          taken[k] = false;
          holder[k] = -1;
        }
      }

      if (inst->result < 0 || escapes[inst->result]) continue;
      /* Comparison results live in memory; see the encoder. */
      if (inst->op == IR_LT || inst->op == IR_LE || inst->op == IR_GT ||
          inst->op == IR_GE || inst->op == IR_EQ || inst->op == IR_NE) {
        continue;
      }
      if (lastUse[inst->result] < 0) continue; /* never read: no register needed */

      for (int k = 0; k < ALLOC_POOL_SIZE; k++) {
        if (taken[k]) continue;
        taken[k] = true;
        holder[k] = inst->result;
        assignment[inst->result] = allocPool(k);
        break;
      }
    }
  }

  free(definedIn);
  free(escapes);
  free(lastUse);
  return assignment;
}

/* The register holding a value: its own, or a scratch one it is loaded into. */
static int readOperand(Encoder *encoder, const int *home, int value, int scratch) {
  if (home[value] >= 0) return home[value];
  ldrDouble(encoder, scratch, REG_SCRATCH, value * 8);
  return scratch;
}

JitCode *csJitCompile(const IrFunction *ir, const char **why) {
  *why = NULL;
#ifndef CS_JIT_APPLE_SILICON
  *why = "no code generator for this architecture yet";
  return NULL;
#else
  Encoder encoder;
  memset(&encoder, 0, sizeof encoder);

  int *home = allocateRegisters(ir);
  int *blockStart = (int *)malloc(sizeof(int) * (size_t)ir->blockCount);
  Fixup *fixups = NULL;
  int fixupCount = 0, fixupCapacity = 0;

  for (int b = 0; b < ir->blockCount; b++) {
    blockStart[b] = encoder.count;

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      switch (inst->op) {
        case IR_CONST: {
          /* A number's Value is its double, so the bits go straight across. */
          uint64_t bits;
          memcpy(&bits, &inst->constant, sizeof bits);
          movImmediate(&encoder, REG_TEMP, bits);
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          fmovToDouble(&encoder, destination, REG_TEMP);
          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_LOAD_LOCAL: {
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          ldrDouble(&encoder, destination, REG_SLOTS, inst->a * 8);
          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_STORE_LOCAL: {
          int source = readOperand(&encoder, home, inst->b, ALLOC_FIRST_SCRATCH);
          strDouble(&encoder, source, REG_SLOTS, inst->a * 8);
          break;
        }

        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: {
          int left = readOperand(&encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
          int right = readOperand(&encoder, home, inst->b, ALLOC_SECOND_SCRATCH);
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          if (inst->op == IR_ADD) fadd(&encoder, destination, left, right);
          else if (inst->op == IR_SUB) fsub(&encoder, destination, left, right);
          else if (inst->op == IR_MUL) fmul(&encoder, destination, left, right);
          else fdiv(&encoder, destination, left, right);
          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_NEG: {
          int operand = readOperand(&encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          fneg(&encoder, destination, operand);
          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_MOD:
          /* fmod is a libm call, and calling out needs a frame this does not
           * set up yet. Left to the interpreter. */
          *why = "modulo";
          goto unsupported;

        case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE: {
          /* The result is materialised as a boolean Value rather than left in
           * the flags.
           *
           * Keeping it in flags would be better and is what a peephole would
           * later do, but it only works when the branch is the very next
           * instruction — and it is not, because the IR round-trips every
           * value through its frame slot. Encoding the general case first
           * means every comparison compiles; fusing them is an optimisation
           * with its own measurement, not a prerequisite. */
          int left = readOperand(&encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
          int right = readOperand(&encoder, home, inst->b, ALLOC_SECOND_SCRATCH);
          fcmp(&encoder, left, right);

          uint64_t trueBits, falseBits;
          Value yes = BOOL_VAL(true), no = BOOL_VAL(false);
          memcpy(&trueBits, &yes, sizeof trueBits);
          memcpy(&falseBits, &no, sizeof falseBits);
          movImmediate(&encoder, 10, trueBits);
          movImmediate(&encoder, 11, falseBits);
          csel(&encoder, REG_TEMP, 10, 11, conditionFor(inst->op));
          /* A boolean stays in a general register and goes to memory. Giving
           * it a floating-point home would mean moving it across the register
           * files here and back again at the branch — two instructions to
           * avoid one store, which measured worse. */
          strGeneral(&encoder, REG_TEMP, REG_SCRATCH, inst->result * 8);
          break;
        }

        case IR_BRANCH: {
          uint64_t trueBits;
          Value yes = BOOL_VAL(true);
          memcpy(&trueBits, &yes, sizeof trueBits);
          ldrGeneral(&encoder, REG_TEMP, REG_SCRATCH, inst->a * 8);
          movImmediate(&encoder, 10, trueBits);
          cmpGeneral(&encoder, REG_TEMP, 10);

          if (fixupCapacity < fixupCount + 2) {
            fixupCapacity = fixupCapacity < 8 ? 8 : fixupCapacity * 2;
            fixups = (Fixup *)realloc(fixups, sizeof(Fixup) * (size_t)fixupCapacity);
          }
          fixups[fixupCount++] = (Fixup){encoder.count, inst->b, true, COND_EQ};
          word(&encoder, 0x54000000u);
          fixups[fixupCount++] = (Fixup){encoder.count, inst->c, false, 0};
          word(&encoder, 0x14000000u);
          break;
        }

        case IR_JUMP:
          if (fixupCapacity < fixupCount + 1) {
            fixupCapacity = fixupCapacity < 8 ? 8 : fixupCapacity * 2;
            fixups = (Fixup *)realloc(fixups, sizeof(Fixup) * (size_t)fixupCapacity);
          }
          fixups[fixupCount++] = (Fixup){encoder.count, inst->a, false, 0};
          word(&encoder, 0x14000000u);
          break;

        case IR_RETURN: {
          int value = readOperand(&encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
          fmovToGeneral(&encoder, 0, value); /* the Value's bits */
          ret(&encoder);
          break;
        }
      }

      if (encoder.failed) {
        *why = "an offset too large to encode";
        goto unsupported;
      }
    }
  }

  /* Falling off the last block returns undefined rather than running on. */
  movImmediate(&encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
  word(&encoder, 0xAA0903E0u); /* mov x0, x9 */
  ret(&encoder);

  for (int f = 0; f < fixupCount; f++) {
    int target = blockStart[fixups[f].block];
    int delta = target - fixups[f].at;
    if (fixups[f].conditional) {
      if (delta < -(1 << 18) || delta >= (1 << 18)) { *why = "branch out of range"; goto unsupported; }
      encoder.words[fixups[f].at] =
          0x54000000u | (((uint32_t)delta & 0x7ffffu) << 5) | fixups[f].condition;
    } else {
      if (delta < -(1 << 25) || delta >= (1 << 25)) { *why = "branch out of range"; goto unsupported; }
      encoder.words[fixups[f].at] = 0x14000000u | ((uint32_t)delta & 0x3ffffffu);
    }
  }

  JitCode *code = publish(&encoder, ir->registerCount);
  free(home);
  free(blockStart);
  free(fixups);
  free(encoder.words);
  if (code == NULL) *why = "could not map executable memory";
  return code;

unsupported:
  free(home);
  free(blockStart);
  free(fixups);
  free(encoder.words);
  return NULL;
#endif
}
