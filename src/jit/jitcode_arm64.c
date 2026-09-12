/* jitcode_arm64.c — the arm64 instructions this backend can emit.
 *
 * Only the handful it needs. Each is written out rather than built by a
 * general assembler, because a general assembler is a project of its own and
 * thirty instructions are not — and because an encoding written by hand next
 * to the manual's field layout is checkable, while one produced by a layer of
 * abstraction is not.
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

/* ---- the buffer -------------------------------------------------------- */

void csJitWord(Encoder *encoder, uint32_t instruction) {
  if (encoder->capacity < encoder->count + 1) {
    encoder->capacity = encoder->capacity < 64 ? 64 : encoder->capacity * 2;
    encoder->words = (uint32_t *)realloc(encoder->words, sizeof(uint32_t) * (size_t)encoder->capacity);
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
void csJitLdrDouble(Encoder *encoder, int destination, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    encoder->failed = true;
    return;
  }
  csJitWord(encoder, 0xFD400000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) | (uint32_t)destination);
}

/* STR <Dt>, [<Xn>, #imm] */
void csJitStrDouble(Encoder *encoder, int source, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    encoder->failed = true;
    return;
  }
  csJitWord(encoder, 0xFD000000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) | (uint32_t)source);
}

void csJitFadd(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x1E602800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFsub(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x1E603800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFmul(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x1E600800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFdiv(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x1E601800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFneg(Encoder *e, int d, int n) {
  csJitWord(e, 0x1E614000u | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFcmp(Encoder *e, int n, int m) {
  csJitWord(e, 0x1E602000u | ((uint32_t)m << 16) | ((uint32_t)n << 5));
}

/* MOVZ/MOVK <Xd>, #imm16, LSL #shift — a 64-bit constant in four pieces. */
void csJitMovImmediate(Encoder *e, int destination, uint64_t value) {
  csJitWord(e, 0xD2800000u | ((uint32_t)(value & 0xffff) << 5) | (uint32_t)destination);
  for (int part = 1; part < 4; part++) {
    uint32_t piece = (uint32_t)((value >> (16 * part)) & 0xffff);
    if (piece == 0) continue;
    csJitWord(e, 0xF2800000u | ((uint32_t)part << 21) | (piece << 5) | (uint32_t)destination);
  }
}

/* FMOV <Dd>, <Xn> and FMOV <Xd>, <Dn> — moving bits between the register
 * files, which is all that is needed because a number's Value is its double. */
void csJitFmovToDouble(Encoder *e, int d, int n) {
  csJitWord(e, 0x9E670000u | ((uint32_t)n << 5) | (uint32_t)d);
}
void csJitFmovToGeneral(Encoder *e, int d, int n) {
  csJitWord(e, 0x9E660000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* LDR <Xt>, [<Xn>, #imm] — the general-register form, for booleans, which are
 * NaN-boxed singletons rather than doubles. */
void csJitLdrGeneral(Encoder *e, int destination, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    e->failed = true;
    return;
  }
  csJitWord(e, 0xF9400000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) | (uint32_t)destination);
}

/* FCVTZS <Xd>, <Dn> — a double truncated toward zero into a 64-bit integer. */
void csJitFcvtzs(Encoder *e, int d, int n) {
  csJitWord(e, 0x9E780000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* SCVTF <Dd>, <Xn> — and back again. Round-tripping a double through these
 * two and comparing is how the integer test below decides exactness: it is
 * true precisely when the value has no fractional part and fits. */
void csJitScvtf(Encoder *e, int d, int n) {
  csJitWord(e, 0x9E620000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* SDIV <Xd>, <Xn>, <Xm> */
void csJitSdiv(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x9AC00C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}

/* MSUB <Xd>, <Xn>, <Xm>, <Xa> — a - n*m, which with a=dividend gives the
 * remainder once the quotient is in hand. */
void csJitMsub(Encoder *e, int d, int n, int m, int a) {
  csJitWord(e, 0x9B008000u | ((uint32_t)m << 16) | ((uint32_t)a << 10) | ((uint32_t)n << 5) | (uint32_t)d);
}

/* CBZ <Xt>, . — the offset is patched by the caller, which knows it. */
int csJitCbzHere(Encoder *e, int t) {
  int at = e->count;
  csJitWord(e, 0xB4000000u | (uint32_t)t);
  return at;
}

/* B.<cond> . and B . — likewise placeholders, patched once their target is
 * known. Local to one instruction's emission, so the distance is never large
 * enough to overflow the field. */
int csJitBranchHere(Encoder *e, uint32_t condition) {
  int at = e->count;
  csJitWord(e, 0x54000000u | condition);
  return at;
}

int csJitJumpHere(Encoder *e) {
  int at = e->count;
  csJitWord(e, 0x14000000u);
  return at;
}

/* Points a placeholder emitted by one of the three above at the current end. */
void csJitPatchToHere(Encoder *e, int at) {
  if (at < 0 || at >= e->count) return;
  uint32_t delta = (uint32_t)(e->count - at);
  if ((e->words[at] & 0xFC000000u) == 0x14000000u) {
    e->words[at] |= delta & 0x3ffffffu; /* B: imm26 */
  } else {
    e->words[at] |= (delta & 0x7ffffu) << 5; /* B.cond and CBZ: imm19 */
  }
}

/* AND <Xd>, <Xn>, <Xm> — the register form. The immediate form encodes only
 * the bitmask patterns arm64 can describe in thirteen bits, and the NaN-box
 * pointer mask is not one of them, so the mask is materialised first. */
void csJitAndRegisters(Encoder *e, int d, int n, int m) {
  csJitWord(e, 0x8A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}

void csJitStrGeneral(Encoder *e, int source, int base, int byteOffset) {
  if (byteOffset < 0 || byteOffset % 8 != 0 || byteOffset / 8 > 4095) {
    e->failed = true;
    return;
  }
  csJitWord(e, 0xF9000000u | ((uint32_t)(byteOffset / 8) << 10) | ((uint32_t)base << 5) | (uint32_t)source);
}

/* STR <Wt>, [<Xn>] — one 32-bit word, for writing the exit index back through
 * the caller's `int *`. */
void csJitStrWord32(Encoder *e, int source, int base) {
  csJitWord(e, 0xB9000000u | ((uint32_t)base << 5) | (uint32_t)source);
}

/* CMP <Xn>, <Xm> */
void csJitCmpGeneral(Encoder *e, int n, int m) {
  csJitWord(e, 0xEB00001Fu | ((uint32_t)m << 16) | ((uint32_t)n << 5));
}

/* CSEL <Xd>, <Xn>, <Xm>, cond — d = cond ? n : m */
void csJitCsel(Encoder *e, int d, int n, int m, uint32_t condition) {
  csJitWord(e, 0x9A800000u | ((uint32_t)m << 16) | (condition << 12) | ((uint32_t)n << 5) | (uint32_t)d);
}

/* FMOV <Dd>, <Dn> — a register-to-register move. */
void csJitFmovDouble(Encoder *e, int d, int n) {
  csJitWord(e, 0x1E604000u | ((uint32_t)n << 5) | (uint32_t)d);
}

/* d2, free in either pool, for the one argument arrangement that is a genuine
 * swap and cannot be done with two moves. */

/* x28 holds the address of the one function this backend calls, materialised
 * on entry rather than at every call — four instructions an iteration for a
 * value that never changes is the same waste hoisting the constants fixed. */

static void ret(Encoder *e) {
  csJitWord(e, 0xD65F03C0u);
}

/* MOV <Xd>, <Xn> — really ORR <Xd>, XZR, <Xn>. */
void csJitMovRegister(Encoder *e, int destination, int source) {
  csJitWord(e, 0xAA0003E0u | ((uint32_t)source << 16) | (uint32_t)destination);
}

/* STP/LDP with a signed offset, and the pre/post-indexed forms the frame needs.
 * `byteOffset` is in bytes and must be a multiple of eight. */
static void pairOp(Encoder *e, uint32_t base, int first, int second, int byteOffset) {
  int scaled = byteOffset / 8;
  if (byteOffset % 8 != 0 || scaled < -64 || scaled > 63) {
    e->failed = true;
    return;
  }
  csJitWord(e, base | (((uint32_t)scaled & 0x7fu) << 15) | ((uint32_t)second << 10) | (31u << 5) | (uint32_t)first);
}

static void storePair(Encoder *e, int a, int b, int at) {
  pairOp(e, 0xA9000000u, a, b, at);
}
static void loadPair(Encoder *e, int a, int b, int at) {
  pairOp(e, 0xA9400000u, a, b, at);
}
static void storePairDouble(Encoder *e, int a, int b, int at) {
  pairOp(e, 0x6D000000u, a, b, at);
}
static void loadPairDouble(Encoder *e, int a, int b, int at) {
  pairOp(e, 0x6D400000u, a, b, at);
}

/* The frame this code keeps.
 *
 * Its three arguments and the addresses it bakes for globals are needed for
 * the whole run, so they live in callee-saved registers rather than the ones a
 * call would clobber — which is what makes calling out possible at all. The
 * floating-point bank d8–d15 is saved for the same reason: a function that
 * calls anything allocates only from there, so the call clobbers nothing that
 * is live and no value has to be spilled around it. */
#define CS_JIT_FRAME_SIZE 160

void csJitEmitPrologue(Encoder *e) {
  pairOp(e, 0xA9800000u, 29, 30, -CS_JIT_FRAME_SIZE); /* stp x29, x30, [sp, #-160]! */
  /* ADD x29, sp, #0 — `mov` from the stack pointer has to be the add form,
   * because register 31 means the zero register in a logical instruction. */
  csJitWord(e, 0x910003FDu);
  storePair(e, 19, 20, 16);
  storePair(e, 21, 22, 32);
  storePair(e, 23, 24, 48);
  storePair(e, 25, 26, 64);
  storePair(e, 27, 28, 80);
  storePairDouble(e, 8, 9, 96);
  storePairDouble(e, 10, 11, 112);
  storePairDouble(e, 12, 13, 128);
  storePairDouble(e, 14, 15, 144);

  csJitMovRegister(e, 19, 0); /* the frame slots */
  csJitMovRegister(e, 20, 1); /* the scratch array */
  csJitMovRegister(e, 21, 2); /* where to report an exit */
  csJitMovImmediate(e, REG_CALL_TARGET, (uint64_t)(uintptr_t)fmod);
}

void csJitEmitEpilogue(Encoder *e) {
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
