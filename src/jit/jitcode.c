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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cscript/jitcode.h"
#include "cscript/object.h"

/* NaN boxing is not an optimisation here, it is the premise.
 *
 * Every load in the generated code moves a slot straight into a floating-point
 * register because a number's Value *is* its double, bit for bit. Under the
 * tagged union a Value is a 16-byte struct with a separate tag, so that is
 * simply false: each load would need unboxing and each store re-boxing, and
 * none of the encoder below would be correct. The `tagged` build therefore
 * gets no backend rather than a broken one. */
#if defined(__APPLE__) && defined(__arm64__) && CS_NAN_BOXING
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

/* AND <Xd>, <Xn>, <Xm> — the register form. The immediate form encodes only
 * the bitmask patterns arm64 can describe in thirteen bits, and the NaN-box
 * pointer mask is not one of them, so the mask is materialised first. */
/* FCVTZS <Xd>, <Dn> — a double truncated toward zero into a 64-bit integer. */
static void fcvtzs(Encoder *e, int d, int n) {
  word(e, 0x9E780000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* SCVTF <Dd>, <Xn> — and back again. Round-tripping a double through these
 * two and comparing is how the integer test below decides exactness: it is
 * true precisely when the value has no fractional part and fits. */
static void scvtf(Encoder *e, int d, int n) {
  word(e, 0x9E620000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* SDIV <Xd>, <Xn>, <Xm> */
static void sdiv(Encoder *e, int d, int n, int m) {
  word(e, 0x9AC00C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}

/* MSUB <Xd>, <Xn>, <Xm>, <Xa> — a - n*m, which with a=dividend gives the
 * remainder once the quotient is in hand. */
static void msub(Encoder *e, int d, int n, int m, int a) {
  word(e, 0x9B008000u | ((uint32_t)m << 16) | ((uint32_t)a << 10) |
              ((uint32_t)n << 5) | (uint32_t)d);
}

/* CBZ <Xt>, . — the offset is patched by the caller, which knows it. */
static int cbzHere(Encoder *e, int t) {
  int at = e->count;
  word(e, 0xB4000000u | (uint32_t)t);
  return at;
}

/* B.<cond> . and B . — likewise placeholders, patched once their target is
 * known. Local to one instruction's emission, so the distance is never large
 * enough to overflow the field. */
static int branchHere(Encoder *e, uint32_t condition) {
  int at = e->count;
  word(e, 0x54000000u | condition);
  return at;
}

static int jumpHere(Encoder *e) {
  int at = e->count;
  word(e, 0x14000000u);
  return at;
}

/* Points a placeholder emitted by one of the three above at the current end. */
static void patchToHere(Encoder *e, int at) {
  if (at < 0 || at >= e->count) return;
  uint32_t delta = (uint32_t)(e->count - at);
  if ((e->words[at] & 0xFC000000u) == 0x14000000u) {
    e->words[at] |= delta & 0x3ffffffu; /* B: imm26 */
  } else {
    e->words[at] |= (delta & 0x7ffffu) << 5; /* B.cond and CBZ: imm19 */
  }
}

static void andRegisters(Encoder *e, int d, int n, int m) {
  word(e, 0x8A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}

static void strGeneral(Encoder *e, int source, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    e->failed = true;
    return;
  }
  word(e, 0xF9000000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) |
              (uint32_t)source);
}

/* STR <Wt>, [<Xn>] — one 32-bit word, for writing the exit index back through
 * the caller's `int *`. */
static void strWord32(Encoder *e, int source, int base) {
  word(e, 0xB9000000u | ((uint32_t)base << 5) | (uint32_t)source);
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

/* FMOV <Dd>, <Dn> — a register-to-register move. */
static void fmovDouble(Encoder *e, int d, int n) {
  word(e, 0x1E604000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* d2, free in either pool, for the one argument arrangement that is a genuine
 * swap and cannot be done with two moves. */
#define CALL_SHUFFLE 2

/* x28 holds the address of the one function this backend calls, materialised
 * on entry rather than at every call — four instructions an iteration for a
 * value that never changes is the same waste hoisting the constants fixed. */
#define REG_CALL_TARGET 28

static void ret(Encoder *e) { word(e, 0xD65F03C0u); }

/* MOV <Xd>, <Xn> — really ORR <Xd>, XZR, <Xn>. */
static void movRegister(Encoder *e, int destination, int source) {
  word(e, 0xAA0003E0u | ((uint32_t)source << 16) | (uint32_t)destination);
}

/* STP/LDP with a signed offset, and the pre/post-indexed forms the frame needs.
 * `byteOffset` is in bytes and must be a multiple of eight. */
static void pairOp(Encoder *e, uint32_t base, int first, int second, int byteOffset) {
  int scaled = byteOffset / 8;
  if (byteOffset % 8 != 0 || scaled < -64 || scaled > 63) {
    e->failed = true;
    return;
  }
  word(e, base | (((uint32_t)scaled & 0x7fu) << 15) | ((uint32_t)second << 10) |
              (31u << 5) | (uint32_t)first);
}

static void storePair(Encoder *e, int a, int b, int at) { pairOp(e, 0xA9000000u, a, b, at); }
static void loadPair(Encoder *e, int a, int b, int at) { pairOp(e, 0xA9400000u, a, b, at); }
static void storePairDouble(Encoder *e, int a, int b, int at) { pairOp(e, 0x6D000000u, a, b, at); }
static void loadPairDouble(Encoder *e, int a, int b, int at) { pairOp(e, 0x6D400000u, a, b, at); }

/* The frame this code keeps.
 *
 * Its three arguments and the addresses it bakes for globals are needed for
 * the whole run, so they live in callee-saved registers rather than the ones a
 * call would clobber — which is what makes calling out possible at all. The
 * floating-point bank d8–d15 is saved for the same reason: a function that
 * calls anything allocates only from there, so the call clobbers nothing that
 * is live and no value has to be spilled around it. */
#define CS_JIT_FRAME_SIZE 160

static void emitPrologue(Encoder *e) {
  pairOp(e, 0xA9800000u, 29, 30, -CS_JIT_FRAME_SIZE); /* stp x29, x30, [sp, #-160]! */
  /* ADD x29, sp, #0 — `mov` from the stack pointer has to be the add form,
   * because register 31 means the zero register in a logical instruction. */
  word(e, 0x910003FDu);
  storePair(e, 19, 20, 16);
  storePair(e, 21, 22, 32);
  storePair(e, 23, 24, 48);
  storePair(e, 25, 26, 64);
  storePair(e, 27, 28, 80);
  storePairDouble(e, 8, 9, 96);
  storePairDouble(e, 10, 11, 112);
  storePairDouble(e, 12, 13, 128);
  storePairDouble(e, 14, 15, 144);

  movRegister(e, 19, 0); /* the frame slots */
  movRegister(e, 20, 1); /* the scratch array */
  movRegister(e, 21, 2); /* where to report an exit */
  movImmediate(e, REG_CALL_TARGET, (uint64_t)(uintptr_t)fmod);
}

static void emitEpilogue(Encoder *e) {
  loadPairDouble(e, 14, 15, 144);
  loadPairDouble(e, 12, 13, 128);
  loadPairDouble(e, 10, 11, 112);
  loadPairDouble(e, 8, 9, 96);
  loadPair(e, 27, 28, 80);
  loadPair(e, 25, 26, 64);
  loadPair(e, 23, 24, 48);
  loadPair(e, 21, 22, 32);
  loadPair(e, 19, 20, 16);
  pairOp(e, 0xA8C00000u, 29, 30, CS_JIT_FRAME_SIZE); /* ldp x29, x30, [sp], #160 */
  ret(e);
}

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

/* ---- IR to machine code ------------------------------------------------- */

/* x0 holds the frame slots, x1 the scratch array for IR values. d0 and d1 are
 * the working registers; d2 holds a materialised constant. Nothing is kept in
 * a register across an instruction, which is the simplification a register
 * allocator would later undo. */
#define REG_SLOTS 19
#define REG_SCRATCH 20
#define REG_EXIT 21 /* where the third argument was put */
/* x3..x8 hold the address of one global each, materialised on entry. */
#define REG_FIRST_GLOBAL 22
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

/* A function that calls out allocates only from d8–d15.
 *
 * Those are the registers a call is obliged to preserve, so nothing live has
 * to be spilled around one — which is the whole reason calling out is cheap
 * enough to be worth doing. It costs fourteen registers, and a loop that fits
 * in eight is most of them. */
#define ALLOC_CALLSAFE_POOL_SIZE 8

static int poolSize(bool callSafe) {
  return callSafe ? ALLOC_CALLSAFE_POOL_SIZE : ALLOC_POOL_SIZE;
}

static int allocPool(int index, bool callSafe) {
  if (callSafe) return 8 + index;
  /* d2..d7, then d16..d31 — skipping the callee-saved bank entirely. */
  return index < 6 ? 2 + index : 16 + (index - 6);
}

/* Which frame slots can live in a register for the whole function.
 *
 * This is where a loop stops touching memory. A counter and an accumulator are
 * frame locals, so without it every iteration loads and stores them exactly as
 * the interpreter does — which is why register allocation alone left the loop
 * benchmark unchanged.
 *
 * A slot qualifies when every value ever stored into it is a proved number.
 * Anything else keeps its memory home: a boolean is a NaN-boxed singleton, and
 * while moving one through a floating-point register happens to preserve its
 * bits, deriving that from the hardware manual is a worse foundation than
 * simply not doing it.
 *
 * Nothing else can observe these slots. The array belongs to the call, the
 * compiled code makes no calls of its own, and the IR refuses any function
 * that captures a local — so there is nothing to write back. */
static bool *promotableSlots(const IrFunction *ir) {
  bool *promotable = (bool *)malloc(sizeof(bool) * (size_t)(ir->slotCount + 1));
  for (int s = 0; s <= ir->slotCount; s++) promotable[s] = true;

  /* Slot 0 is the callee and the arguments follow it: those arrive as Values
   * the caller wrote, and only a parameter the checker typed is known. */
  promotable[0] = false;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op != IR_STORE_LOCAL) continue;
      if (inst->a < 0 || inst->a > ir->slotCount) continue;
      if (inst->b < 0 || inst->b >= ir->registerCount ||
          ir->registerTypes[inst->b] != IR_TYPE_NUMBER) {
        promotable[inst->a] = false;
      }
    }
  }

  /* A parameter is only known to be a number if it was declared one. */
  for (int s = 1; s <= ir->slotCount; s++) {
    if (ir->slotTypes != NULL && s <= ir->slotCount &&
        ir->slotTypes[s] != IR_TYPE_NUMBER) {
      /* Still promotable if nothing outside the function put a value there —
       * that is, if it is a temporary rather than an argument. */
      if (s <= ir->source->arity) promotable[s] = false;
    }
  }
  return promotable;
}

/* -1 means the value lives in the scratch array rather than a register. */
static int *allocateRegisters(const IrFunction *ir, int reserved, bool callSafe) {
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
      /* `a` is not a value for these: a slot, a block, or — for an exit — a
       * bytecode offset and a stack height. */
      if (inst->op == IR_LOAD_LOCAL || inst->op == IR_JUMP) operands[0] = -1;
      if (inst->op == IR_STORE_LOCAL || inst->op == IR_EXIT) operands[0] = -1;
      if (inst->op == IR_LOAD_GLOBAL || inst->op == IR_STORE_GLOBAL) operands[0] = -1;
      if (inst->op == IR_EXIT || inst->op == IR_LOAD_GLOBAL) operands[1] = -1;
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

    int pool = poolSize(callSafe);
    bool taken[ALLOC_POOL_SIZE];
    int holder[ALLOC_POOL_SIZE];
    for (int k = 0; k < pool; k++) { taken[k] = false; holder[k] = -1; }

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      /* Free anything whose last use was the previous instruction. */
      for (int k = 0; k < pool; k++) {
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

      for (int k = 0; k < pool - reserved; k++) {
        if (taken[k]) continue;
        taken[k] = true;
        holder[k] = inst->result;
        assignment[inst->result] = allocPool(k, callSafe);
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

/* Whether every block can be entered with nothing live in a register.
 *
 * Registers in this IR are operand-stack positions, so a register live across
 * a block boundary means the operand stack was not empty there. Where no block
 * reads a register it did not itself write, all of a function's live state is
 * in its slots — and the interpreter and the compiled code agree, byte for
 * byte, on where those are. That agreement is what makes on-stack replacement
 * a jump rather than a translation, so it is checked rather than assumed. */
static bool blocksAreSelfContained(const IrFunction *ir) {
  bool *written = (bool *)calloc((size_t)ir->registerCount + 1, sizeof(bool));
  bool clean = true;

  for (int b = 0; b < ir->blockCount && clean; b++) {
    memset(written, 0, sizeof(bool) * ((size_t)ir->registerCount + 1));

    for (int i = 0; i < ir->blocks[b].count && clean; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      int reads[2];
      csIrRegisterOperands(inst, &reads[0], &reads[1]);

      for (int r = 0; r < 2; r++) {
        if (reads[r] >= 0 && reads[r] <= ir->registerCount && !written[reads[r]]) {
          clean = false;
        }
      }
      if (inst->result >= 0 && inst->result <= ir->registerCount) {
        written[inst->result] = true;
      }
    }
  }

  free(written);
  return clean;
}

/* A block reached by an edge that does not move forward is a loop header. */
static bool isLoopHeader(const IrFunction *ir, int target) {
  for (int b = target; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op == IR_JUMP && inst->a == target) return true;
      if (inst->op == IR_BRANCH && (inst->b == target || inst->c == target)) return true;
    }
  }
  return false;
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
      if (ir->blocks[b].instructions[i].op == IR_MOD) { callSafe = true; break; }
    }
  }
  int pool = poolSize(callSafe);

  bool *promotable = promotableSlots(ir);
  int *slotHome = (int *)malloc(sizeof(int) * (size_t)(ir->slotCount + 1));
  int promotedCount = 0;
  for (int s = 0; s <= ir->slotCount; s++) {
    slotHome[s] = -1;
    if (promotable[s] && promotedCount < pool / 2) {
      slotHome[s] = allocPool(pool - 1 - promotedCount, callSafe);
      promotedCount++;
    }
  }
  free(promotable);
  if (getenv("CS_JIT_DUMP_IR") != NULL) {
    printf("  compiling %s: %d of %d slots promoted to registers\n",
           ir->source->name != NULL ? ir->source->name->chars : "<top level>",
           promotedCount, ir->slotCount + 1);
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
        if (constantValue[k] == bits) { seen = true; break; }
      }
      if (seen) continue;
      if (promotedCount + constantCount >= pool / 2) break;
      constantValue[constantCount] = bits;
      constantHome[constantCount] =
          allocPool(pool - 1 - promotedCount - constantCount, callSafe);
      constantCount++;
    }
  }

  /* One address register per distinct global. Resolved here, while the
   * program is running and the binding certainly exists — the function only
   * reached this point by being hot, which is after its module ran. */
  int globalName[CS_JIT_MAX_GLOBALS];
  Value *globalAddress[CS_JIT_MAX_GLOBALS];
  int globalCount = 0;
  Table *globalTable =
      ir->source->module != NULL ? &ir->source->module->globals : NULL;

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op != IR_LOAD_GLOBAL && inst->op != IR_STORE_GLOBAL) continue;

      bool seen = false;
      for (int g = 0; g < globalCount; g++) {
        if (globalName[g] == inst->a) { seen = true; break; }
      }
      if (seen) continue;
      if (globalCount >= CS_JIT_MAX_GLOBALS || globalTable == NULL) {
        *why = "more globals than there are registers to hold them";
        return NULL;
      }

      Value name = ir->source->chunk.constants.values[inst->a];
      Entry *entry = IS_STRING(name) ? csTableFindEntry(globalTable, AS_STRING(name))
                                     : NULL;
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

  int *home = allocateRegisters(ir, promotedCount + constantCount, callSafe);
  int *blockStart = (int *)malloc(sizeof(int) * (size_t)ir->blockCount);

  emitPrologue(&encoder);

  /* Load the promoted slots and materialise the constants, once, on entry. */
  for (int s = 0; s <= ir->slotCount; s++) {
    if (slotHome[s] >= 0) ldrDouble(&encoder, slotHome[s], REG_SLOTS, s * 8);
  }
  for (int k = 0; k < constantCount; k++) {
    movImmediate(&encoder, REG_TEMP, constantValue[k]);
    fmovToDouble(&encoder, constantHome[k], REG_TEMP);
  }
  for (int g = 0; g < globalCount; g++) {
    movImmediate(&encoder, REG_FIRST_GLOBAL + g, (uint64_t)(uintptr_t)globalAddress[g]);
  }
  Fixup *fixups = NULL;
  int fixupCount = 0, fixupCapacity = 0;

  for (int b = 0; b < ir->blockCount; b++) {
    blockStart[b] = encoder.count;

    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];

      switch (inst->op) {
        case IR_STORE_PROPERTY: {
          /* The load's address computation, then a store instead of a load.
           * The property already exists in the shape checked at entry, so this
           * cannot grow the object, and the collector is not generational so
           * there is no barrier to emit. */
          if (slotHome[inst->a] >= 0) {
            *why = "a property write on a slot held as a number";
            goto unsupported;
          }

          int value = readOperand(&encoder, home, inst->b, ALLOC_FIRST_SCRATCH);

          ldrGeneral(&encoder, REG_TEMP, REG_SLOTS, inst->a * 8);
          movImmediate(&encoder, 10, ~(CS_SIGN_BIT | CS_QNAN));
          andRegisters(&encoder, REG_TEMP, REG_TEMP, 10);
          ldrGeneral(&encoder, REG_TEMP, REG_TEMP,
                     (int)offsetof(ObjObject, as.slots.values));
          strDouble(&encoder, value, REG_TEMP, inst->c * 8);
          break;
        }

        case IR_LOAD_PROPERTY: {
          /* `slots[a].<property b>`, with no guard on it.
           *
           * What makes that safe is the entry check, not luck: csIrEntryShapes
           * proved before the body started that this slot holds an object of
           * exactly this shape and that the property holds a number, and the
           * lowering only emits this for a slot the function never writes. So
           * the layout cannot have moved between there and here.
           *
           * Three steps: read the Value, strip the NaN-box tag to get the
           * object, follow it to its storage, and index. */
          if (slotHome[inst->a] >= 0) {
            /* The slot is homed in a floating-point register, which means
             * something typed it a number — and an object is not one. The
             * lowering should never produce this pairing; refusing is cheaper
             * than trusting it. */
            *why = "a property read on a slot held as a number";
            goto unsupported;
          }

          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;

          ldrGeneral(&encoder, REG_TEMP, REG_SLOTS, inst->a * 8);
          movImmediate(&encoder, 10, ~(CS_SIGN_BIT | CS_QNAN));
          andRegisters(&encoder, REG_TEMP, REG_TEMP, 10);
          ldrGeneral(&encoder, REG_TEMP, REG_TEMP,
                     (int)offsetof(ObjObject, as.slots.values));
          ldrDouble(&encoder, destination, REG_TEMP, inst->b * 8);

          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_CONST: {
          /* A number's Value is its double, so the bits go straight across. */
          uint64_t bits;
          memcpy(&bits, &inst->constant, sizeof bits);

          int source = -1;
          for (int k = 0; k < constantCount; k++) {
            if (constantValue[k] == bits) { source = constantHome[k]; break; }
          }

          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          if (source >= 0) {
            /* Already in a register from the entry block. */
            if (source != destination) fmovDouble(&encoder, destination, source);
          } else {
            movImmediate(&encoder, REG_TEMP, bits);
            fmovToDouble(&encoder, destination, REG_TEMP);
          }
          if (home[inst->result] < 0) {
            strDouble(&encoder, destination, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_LOAD_GLOBAL: {
          int slotOf = -1;
          for (int g = 0; g < globalCount; g++) {
            if (globalName[g] == inst->a) { slotOf = g; break; }
          }
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          ldrDouble(&encoder, destination, REG_FIRST_GLOBAL + slotOf, 0);
          if (home[inst->result] < 0) {
            strDouble(&encoder, destination, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_STORE_GLOBAL: {
          int slotOf = -1;
          for (int g = 0; g < globalCount; g++) {
            if (globalName[g] == inst->a) { slotOf = g; break; }
          }
          int source = readOperand(&encoder, home, inst->b, ALLOC_FIRST_SCRATCH);
          strDouble(&encoder, source, REG_FIRST_GLOBAL + slotOf, 0);
          break;
        }

        case IR_LOAD_LOCAL: {
          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          if (slotHome[inst->a] >= 0) {
            fmovDouble(&encoder, destination, slotHome[inst->a]);
          } else {
            ldrDouble(&encoder, destination, REG_SLOTS, inst->a * 8);
          }
          if (home[inst->result] < 0) {
            strDouble(&encoder, 0, REG_SCRATCH, inst->result * 8);
          }
          break;
        }

        case IR_STORE_LOCAL: {
          int source = readOperand(&encoder, home, inst->b, ALLOC_FIRST_SCRATCH);
          if (slotHome[inst->a] >= 0) {
            fmovDouble(&encoder, slotHome[inst->a], source);
          } else {
            strDouble(&encoder, source, REG_SLOTS, inst->a * 8);
          }
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

        case IR_MOD: {
          /* JavaScript's `%` is C's fmod, and arm64 has no instruction for it.
           * The obvious inline form, `a - b * trunc(a / b)`, is exact only
           * while the quotient is — past 2^53 it is not — so this calls the
           * real thing rather than being approximately right.
           *
           * Nothing is spilled around the call: a function that reaches here
           * allocated only from d8–d15, which a call must preserve. The two
           * arguments go via d2 and d3 so that moving one into place cannot
           * land on the other. */
          int left = readOperand(&encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
          int right = readOperand(&encoder, home, inst->b, ALLOC_SECOND_SCRATCH);

          /* The integer case, inline, before falling back to the call.
           *
           * The interpreter has taken this path since long before there was a
           * compiler — loop counters are whole numbers virtually always, and
           * integer remainder is about a nanosecond against fmod's twenty. The
           * compiled code did not, which made it *slower at this operation
           * than the interpreter it replaced*: bench/loop_arith calls fmod ten
           * million times and was only a quarter faster compiled for that one
           * reason.
           *
           * Exactness is decided by round-tripping through the integer
           * registers and comparing, which is true precisely when the value
           * has no fractional part and fits in 64 bits. Zero on either side
           * goes to fmod: a zero divisor to get the NaN, and a zero dividend
           * so that `-0 % n` stays -0 rather than becoming +0. */
          int slowPath[4];
          int slowCount = 0;

          fcvtzs(&encoder, REG_TEMP, left);
          scvtf(&encoder, CALL_SHUFFLE, REG_TEMP);
          fcmp(&encoder, CALL_SHUFFLE, left);
          slowPath[slowCount++] = branchHere(&encoder, 0x1u); /* b.ne */

          fcvtzs(&encoder, 10, right);
          scvtf(&encoder, CALL_SHUFFLE, 10);
          fcmp(&encoder, CALL_SHUFFLE, right);
          slowPath[slowCount++] = branchHere(&encoder, 0x1u);

          slowPath[slowCount++] = cbzHere(&encoder, 10);        /* divisor 0 */
          slowPath[slowCount++] = cbzHere(&encoder, REG_TEMP);  /* dividend 0 */

          {
            int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
            sdiv(&encoder, 11, REG_TEMP, 10);
            msub(&encoder, 11, 11, 10, REG_TEMP);
            scvtf(&encoder, destination, 11);
            if (home[inst->result] < 0) {
              strDouble(&encoder, destination, REG_SCRATCH, inst->result * 8);
            }
          }
          int afterFast = jumpHere(&encoder);
          for (int s = 0; s < slowCount; s++) patchToHere(&encoder, slowPath[s]);

          /* Into d0 and d1 without one move landing on the other's source. */
          if (right != 0) {
            if (left != 0) fmovDouble(&encoder, 0, left);
            if (right != 1) fmovDouble(&encoder, 1, right);
          } else if (left != 1) {
            fmovDouble(&encoder, 1, 0);
            fmovDouble(&encoder, 0, left);
          } else {
            fmovDouble(&encoder, CALL_SHUFFLE, 1);
            fmovDouble(&encoder, 1, 0);
            fmovDouble(&encoder, 0, CALL_SHUFFLE);
          }

          word(&encoder, 0xD63F0000u | ((uint32_t)REG_CALL_TARGET << 5)); /* blr x28 */

          int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
          if (destination != 0) fmovDouble(&encoder, destination, 0);
          if (home[inst->result] < 0) {
            strDouble(&encoder, destination, REG_SCRATCH, inst->result * 8);
          }
          patchToHere(&encoder, afterFast);
          break;
        }

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

          /* When the branch is the very next instruction, the answer stays in
           * the flags and never becomes a value at all.
           *
           * This was impossible until dead-store elimination ran: the lowering
           * put a store between the comparison and the branch, so they were
           * never adjacent. Materialising the boolean costs two immediate
           * loads, a csel, a store and a load — on a loop condition, every
           * iteration. */
          const IrInst *following =
              i + 1 < ir->blocks[b].count ? &ir->blocks[b].instructions[i + 1] : NULL;
          if (following != NULL && following->op == IR_BRANCH &&
              following->a == inst->result) {
            if (fixupCapacity < fixupCount + 2) {
              fixupCapacity = fixupCapacity < 8 ? 8 : fixupCapacity * 2;
              fixups = (Fixup *)realloc(fixups, sizeof(Fixup) * (size_t)fixupCapacity);
            }
            fixups[fixupCount++] =
                (Fixup){encoder.count, following->b, true, conditionFor(inst->op)};
            word(&encoder, 0x54000000u);
            fixups[fixupCount++] = (Fixup){encoder.count, following->c, false, 0};
            word(&encoder, 0x14000000u);
            i++; /* the branch went with the comparison */
            break;
          }

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
          emitEpilogue(&encoder);
          break;
        }

        case IR_EXIT: {
          /* Give the frame back. Every promoted slot goes home first: the
           * interpreter reads locals out of the frame and knows nothing about
           * the registers they have been living in. Nothing else needs writing
           * — an exit is only ever emitted at the height its block began at,
           * so every live position is still where the interpreter left it. */
          for (int s = 0; s <= ir->slotCount; s++) {
            if (slotHome[s] >= 0) strDouble(&encoder, slotHome[s], REG_SLOTS, s * 8);
          }

          if (exitCount >= exitCapacity) {
            exitCapacity = exitCapacity < 8 ? 8 : exitCapacity * 2;
            exits = (JitExit *)realloc(exits, sizeof(JitExit) * (size_t)exitCapacity);
          }
          exits[exitCount].bytecodeOffset = inst->a;
          exits[exitCount].stackHeight = inst->b;

          movImmediate(&encoder, REG_TEMP, (uint64_t)exitCount);
          strWord32(&encoder, REG_TEMP, REG_EXIT);
          exitCount++;

          movImmediate(&encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
          word(&encoder, 0xAA0903E0u); /* mov x0, x9 */
          emitEpilogue(&encoder);
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
  emitEpilogue(&encoder);

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
  if (blocksAreSelfContained(ir)) {
    for (int b = 0; b < ir->blockCount && osrCount < CS_JIT_MAX_OSR; b++) {
      if (!isLoopHeader(ir, b)) continue;

      osrBlock[osrCount] = b;
      osrWord[osrCount] = encoder.count;
      osrCount++;

      emitPrologue(&encoder);
      for (int s = 0; s <= ir->slotCount; s++) {
        if (slotHome[s] >= 0) ldrDouble(&encoder, slotHome[s], REG_SLOTS, s * 8);
      }
      for (int k = 0; k < constantCount; k++) {
        movImmediate(&encoder, REG_TEMP, constantValue[k]);
        fmovToDouble(&encoder, constantHome[k], REG_TEMP);
      }
      for (int g = 0; g < globalCount; g++) {
        movImmediate(&encoder, REG_FIRST_GLOBAL + g,
                     (uint64_t)(uintptr_t)globalAddress[g]);
      }

      if (fixupCapacity < fixupCount + 1) {
        fixupCapacity = fixupCapacity < 8 ? 8 : fixupCapacity * 2;
        fixups = (Fixup *)realloc(fixups, sizeof(Fixup) * (size_t)fixupCapacity);
      }
      fixups[fixupCount++] = (Fixup){encoder.count, b, false, 0};
      word(&encoder, 0x14000000u);
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
      if (delta < -(1 << 18) || delta >= (1 << 18)) { *why = "branch out of range"; goto unsupported; }
      encoder.words[fixups[f].at] =
          0x54000000u | (((uint32_t)delta & 0x7ffffu) << 5) | fixups[f].condition;
    } else {
      if (delta < -(1 << 25) || delta >= (1 << 25)) { *why = "branch out of range"; goto unsupported; }
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
