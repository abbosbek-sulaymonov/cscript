/* jitcode_emit.c — one IR instruction to machine code.
 *
 * A switch and nothing else. What each case needs is in EmitAt rather than in
 * the locals of the function that drives it, which is what lets this be a file
 * of its own — and what makes each case readable on its own terms, since
 * nothing here depends on the order the blocks are walked in.
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

/* ---- the two things a case may append to -------------------------------- */

void csJitAddFixup(EmitAt *at, int instructionAt, int block, bool conditional, uint32_t condition) {
  if (*at->fixupCapacity < *at->fixupCount + 1) {
    *at->fixupCapacity = *at->fixupCapacity < 8 ? 8 : *at->fixupCapacity * 2;
    *at->fixups = (Fixup *)realloc(*at->fixups, sizeof(Fixup) * (size_t)*at->fixupCapacity);
  }
  (*at->fixups)[(*at->fixupCount)++] = (Fixup){instructionAt, block, conditional, condition};
}

int csJitAddExit(EmitAt *at, int bytecodeOffset, int stackHeight) {
  if (*at->exitCount >= *at->exitCapacity) {
    *at->exitCapacity = *at->exitCapacity < 8 ? 8 : *at->exitCapacity * 2;
    *at->exits = (JitExit *)realloc(*at->exits, sizeof(JitExit) * (size_t)*at->exitCapacity);
  }
  (*at->exits)[*at->exitCount].bytecodeOffset = bytecodeOffset;
  (*at->exits)[*at->exitCount].stackHeight = stackHeight;
  return (*at->exitCount)++;
}

/* Emits one instruction.
 *
 * Every case is independent of every other: the state that crosses between
 * them — where each value lives, what has been laid out, what still needs
 * patching — is in EmitAt rather than in locals, which is what let this stop
 * being one 623-line function. */
bool csJitEmitInstruction(EmitAt *at) {
#ifndef CS_JIT_APPLE_SILICON
  (void)at;
  return false;
#else
  Encoder *encoder = at->encoder;
  const IrFunction *ir = at->ir;
  const IrInst *inst = at->inst;
  const int *home = at->home;
  const int *slotHome = at->slotHome;
  const uint64_t *constantValue = at->constantValue;
  const int *constantHome = at->constantHome;
  const int constantCount = at->constantCount;
  const int *globalName = at->globalName;
  const int globalCount = at->globalCount;
  const int b = at->block;
  const char **why = at->why;

  switch (inst->op) {
    case IR_STORE_PROPERTY: {
      /* The load's address computation, then a store instead of a load.
       * The property already exists in the shape checked at entry, so this
       * cannot grow the object, and the collector is not generational so
       * there is no barrier to emit. */
      if (slotHome[inst->a] >= 0) {
        *why = "a property write on a slot held as a number";
        return false;
      }

      int value = csJitReadOperand(encoder, home, inst->b, ALLOC_FIRST_SCRATCH);

      csJitLdrGeneral(encoder, REG_TEMP, REG_SLOTS, inst->a * 8);
      csJitMovImmediate(encoder, 10, ~(CS_SIGN_BIT | CS_QNAN));
      csJitAndRegisters(encoder, REG_TEMP, REG_TEMP, 10);
      csJitLdrGeneral(encoder, REG_TEMP, REG_TEMP, (int)offsetof(ObjObject, as.slots.values));
      csJitStrDouble(encoder, value, REG_TEMP, inst->c * 8);
      break;
    }

    case IR_ADD_PROPERTY: {
      /* A store that adds: the same address computation as the one above, and
       * then the layout the object takes on.
       *
       * Nothing is looked up and nothing is allocated. The pair — the layout
       * expected on the way in and the one adopted here — came from the site's
       * cache, the first was checked at entry, and the room for the value was
       * checked there too. What is left is two stores.
       *
       * The value goes in before the shape, because the collector sizes its
       * walk of an object's slots from the shape: the other order would show
       * it a slot the shape counts and nothing has written. */
      if (slotHome[inst->a] >= 0) {
        *why = "a property add on a slot held as a number";
        return false;
      }

      int value = csJitReadOperand(encoder, home, inst->b, ALLOC_FIRST_SCRATCH);

      csJitLdrGeneral(encoder, REG_TEMP, REG_SLOTS, inst->a * 8);
      csJitMovImmediate(encoder, 10, ~(CS_SIGN_BIT | CS_QNAN));
      csJitAndRegisters(encoder, REG_TEMP, REG_TEMP, 10);

      /* The object is needed after the storage pointer is loaded, so the two
       * live in different registers rather than one being recomputed. */
      csJitMovRegister(encoder, 11, REG_TEMP);
      csJitLdrGeneral(encoder, REG_TEMP, REG_TEMP, (int)offsetof(ObjObject, as.slots.values));
      csJitStrDouble(encoder, value, REG_TEMP, inst->c * 8);

      csJitMovImmediate(encoder, 10, (uint64_t)(uintptr_t)AS_OBJ(inst->constant));
      csJitStrGeneral(encoder, 10, 11, (int)offsetof(ObjObject, shape));
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
        return false;
      }

      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;

      csJitLdrGeneral(encoder, REG_TEMP, REG_SLOTS, inst->a * 8);
      csJitMovImmediate(encoder, 10, ~(CS_SIGN_BIT | CS_QNAN));
      csJitAndRegisters(encoder, REG_TEMP, REG_TEMP, 10);
      csJitLdrGeneral(encoder, REG_TEMP, REG_TEMP, (int)offsetof(ObjObject, as.slots.values));
      csJitLdrDouble(encoder, destination, REG_TEMP, inst->b * 8);

      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, 0, REG_SCRATCH, inst->result * 8);
      }
      break;
    }

    case IR_CONST: {
      /* A number's Value is its double, so the bits go straight across. */
      uint64_t bits;
      memcpy(&bits, &inst->constant, sizeof bits);

      int source = -1;
      for (int k = 0; k < constantCount; k++) {
        if (constantValue[k] == bits) {
          source = constantHome[k];
          break;
        }
      }

      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      if (source >= 0) {
        /* Already in a register from the entry block. */
        if (source != destination) csJitFmovDouble(encoder, destination, source);
      } else {
        csJitMovImmediate(encoder, REG_TEMP, bits);
        csJitFmovToDouble(encoder, destination, REG_TEMP);
      }
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, destination, REG_SCRATCH, inst->result * 8);
      }
      break;
    }

    case IR_LOAD_GLOBAL: {
      int slotOf = -1;
      for (int g = 0; g < globalCount; g++) {
        if (globalName[g] == inst->a) {
          slotOf = g;
          break;
        }
      }
      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      csJitLdrDouble(encoder, destination, REG_FIRST_GLOBAL + slotOf, 0);
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, destination, REG_SCRATCH, inst->result * 8);
      }
      break;
    }

    case IR_STORE_GLOBAL: {
      int slotOf = -1;
      for (int g = 0; g < globalCount; g++) {
        if (globalName[g] == inst->a) {
          slotOf = g;
          break;
        }
      }
      int source = csJitReadOperand(encoder, home, inst->b, ALLOC_FIRST_SCRATCH);
      csJitStrDouble(encoder, source, REG_FIRST_GLOBAL + slotOf, 0);
      break;
    }

    case IR_LOAD_LOCAL: {
      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      if (slotHome[inst->a] >= 0) {
        csJitFmovDouble(encoder, destination, slotHome[inst->a]);
      } else {
        csJitLdrDouble(encoder, destination, REG_SLOTS, inst->a * 8);
      }
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, 0, REG_SCRATCH, inst->result * 8);
      }
      break;
    }

    case IR_STORE_LOCAL: {
      int source = csJitReadOperand(encoder, home, inst->b, ALLOC_FIRST_SCRATCH);
      if (slotHome[inst->a] >= 0) {
        csJitFmovDouble(encoder, slotHome[inst->a], source);
      } else {
        csJitStrDouble(encoder, source, REG_SLOTS, inst->a * 8);
      }
      break;
    }

    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_DIV: {
      int left = csJitReadOperand(encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
      int right = csJitReadOperand(encoder, home, inst->b, ALLOC_SECOND_SCRATCH);
      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      if (inst->op == IR_ADD)
        csJitFadd(encoder, destination, left, right);
      else if (inst->op == IR_SUB)
        csJitFsub(encoder, destination, left, right);
      else if (inst->op == IR_MUL)
        csJitFmul(encoder, destination, left, right);
      else
        csJitFdiv(encoder, destination, left, right);
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, 0, REG_SCRATCH, inst->result * 8);
      }
      break;
    }

    case IR_NEG: {
      int operand = csJitReadOperand(encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      csJitFneg(encoder, destination, operand);
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, 0, REG_SCRATCH, inst->result * 8);
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
      int left = csJitReadOperand(encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
      int right = csJitReadOperand(encoder, home, inst->b, ALLOC_SECOND_SCRATCH);

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

      csJitFcvtzs(encoder, REG_TEMP, left);
      csJitScvtf(encoder, CALL_SHUFFLE, REG_TEMP);
      csJitFcmp(encoder, CALL_SHUFFLE, left);
      slowPath[slowCount++] = csJitBranchHere(encoder, 0x1u); /* b.ne */

      csJitFcvtzs(encoder, 10, right);
      csJitScvtf(encoder, CALL_SHUFFLE, 10);
      csJitFcmp(encoder, CALL_SHUFFLE, right);
      slowPath[slowCount++] = csJitBranchHere(encoder, 0x1u);

      slowPath[slowCount++] = csJitCbzHere(encoder, 10);       /* divisor 0 */
      slowPath[slowCount++] = csJitCbzHere(encoder, REG_TEMP); /* dividend 0 */

      {
        int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
        csJitSdiv(encoder, 11, REG_TEMP, 10);
        csJitMsub(encoder, 11, 11, 10, REG_TEMP);
        csJitScvtf(encoder, destination, 11);
        if (home[inst->result] < 0) {
          csJitStrDouble(encoder, destination, REG_SCRATCH, inst->result * 8);
        }
      }
      int afterFast = csJitJumpHere(encoder);
      for (int s = 0; s < slowCount; s++) csJitPatchToHere(encoder, slowPath[s]);

      /* Into d0 and d1 without one move landing on the other's source. */
      if (right != 0) {
        if (left != 0) csJitFmovDouble(encoder, 0, left);
        if (right != 1) csJitFmovDouble(encoder, 1, right);
      } else if (left != 1) {
        csJitFmovDouble(encoder, 1, 0);
        csJitFmovDouble(encoder, 0, left);
      } else {
        csJitFmovDouble(encoder, CALL_SHUFFLE, 1);
        csJitFmovDouble(encoder, 1, 0);
        csJitFmovDouble(encoder, 0, CALL_SHUFFLE);
      }

      csJitWord(encoder, 0xD63F0000u | ((uint32_t)REG_CALL_TARGET << 5)); /* blr x28 */

      int destination = home[inst->result] >= 0 ? home[inst->result] : 0;
      if (destination != 0) csJitFmovDouble(encoder, destination, 0);
      if (home[inst->result] < 0) {
        csJitStrDouble(encoder, destination, REG_SCRATCH, inst->result * 8);
      }
      csJitPatchToHere(encoder, afterFast);
      break;
    }

    case IR_LT:
    case IR_LE:
    case IR_GT:
    case IR_GE:
    case IR_EQ:
    case IR_NE: {
      /* Every comparison here is a floating-point one, so every operand of
       * one has to be a number.
       *
       * For the ordered comparisons that is already settled: csIrIsFullyTyped
       * refuses a function whose `<` has an operand nothing proved numeric,
       * so one never reaches this far. Equality is deliberately *not* in
       * that check — `===` is defined for every type and the IR
       * interpreter answers it with csValuesStrictEqual, which is right
       * for every type. `fcmp` is not. A non-number Value is NaN-boxed,
       * which is to say it is a quiet NaN, so comparing two of them is
       * unordered and every `===` on anything but a number came out
       * false — `s === s` on a string among them.
       *
       * Refusing here rather than in the lowering is what keeps the IR
       * interpreter's coverage: it can run these correctly, and only the
       * encoder cannot. */
      if (inst->op == IR_EQ || inst->op == IR_NE) {
        if (ir->registerTypes[inst->a] != IR_TYPE_NUMBER || ir->registerTypes[inst->b] != IR_TYPE_NUMBER) {
          *why = "an equality whose operands are not both numbers";
          return false;
        }
      }

      /* The result is materialised as a boolean Value rather than left in
       * the flags.
       *
       * Keeping it in flags would be better and is what a peephole would
       * later do, but it only works when the branch is the very next
       * instruction — and it is not, because the IR round-trips every
       * value through its frame slot. Encoding the general case first
       * means every comparison compiles; fusing them is an optimisation
       * with its own measurement, not a prerequisite. */
      int left = csJitReadOperand(encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
      int right = csJitReadOperand(encoder, home, inst->b, ALLOC_SECOND_SCRATCH);
      csJitFcmp(encoder, left, right);

      /* When the branch is the very next instruction, the answer stays in
       * the flags and never becomes a value at all.
       *
       * This was impossible until dead-store elimination ran: the lowering
       * put a store between the comparison and the branch, so they were
       * never adjacent. Materialising the boolean costs two immediate
       * loads, a csel, a store and a load — on a loop condition, every
       * iteration. */
      const IrInst *following = *at->index + 1 < ir->blocks[b].count ? &ir->blocks[b].instructions[*at->index + 1] : NULL;
      if (following != NULL && following->op == IR_BRANCH && following->a == inst->result) {
        csJitAddFixup(at, encoder->count, following->b, true, csJitConditionFor(inst->op));
        csJitWord(encoder, 0x54000000u);
        csJitAddFixup(at, encoder->count, following->c, false, 0);
        csJitWord(encoder, 0x14000000u);
        (*at->index)++; /* the branch went with the comparison */
        break;
      }

      uint64_t trueBits, falseBits;
      Value yes = BOOL_VAL(true), no = BOOL_VAL(false);
      memcpy(&trueBits, &yes, sizeof trueBits);
      memcpy(&falseBits, &no, sizeof falseBits);
      csJitMovImmediate(encoder, 10, trueBits);
      csJitMovImmediate(encoder, 11, falseBits);
      csJitCsel(encoder, REG_TEMP, 10, 11, csJitConditionFor(inst->op));
      /* A boolean stays in a general register and goes to memory. Giving
       * it a floating-point home would mean moving it across the register
       * files here and back again at the branch — two instructions to
       * avoid one store, which measured worse. */
      csJitStrGeneral(encoder, REG_TEMP, REG_SCRATCH, inst->result * 8);
      break;
    }

    case IR_BRANCH: {
      uint64_t trueBits;
      Value yes = BOOL_VAL(true);
      memcpy(&trueBits, &yes, sizeof trueBits);

      /* Wherever the value actually is.
       *
       * A comparison's result is kept in memory on purpose — see the
       * allocator — so reading the scratch array was right for every
       * branch there was, until one came along whose condition was not a
       * comparison. `while (true)` branches on a constant, and a constant
       * does get a register, so the load found whatever the scratch slot
       * held before: the loop was taken or skipped on uninitialised
       * memory. Nothing compiled it until slot types became precise enough
       * for a function containing one to be fully typed. */
      if (home[inst->a] >= 0) {
        csJitFmovToGeneral(encoder, REG_TEMP, home[inst->a]);
      } else {
        csJitLdrGeneral(encoder, REG_TEMP, REG_SCRATCH, inst->a * 8);
      }
      csJitMovImmediate(encoder, 10, trueBits);
      csJitCmpGeneral(encoder, REG_TEMP, 10);

      csJitAddFixup(at, encoder->count, inst->b, true, COND_EQ);
      csJitWord(encoder, 0x54000000u);
      csJitAddFixup(at, encoder->count, inst->c, false, 0);
      csJitWord(encoder, 0x14000000u);
      break;
    }

    case IR_JUMP:
      csJitAddFixup(at, encoder->count, inst->a, false, 0);
      csJitWord(encoder, 0x14000000u);
      break;

    case IR_RETURN: {
      int value = csJitReadOperand(encoder, home, inst->a, ALLOC_FIRST_SCRATCH);
      csJitFmovToGeneral(encoder, 0, value); /* the Value's bits */
      csJitEmitEpilogue(encoder);
      break;
    }

    case IR_CALL: {
      /* A real call: the arguments are already in the slots above the
       * destination, and the helper pushes them where the interpreter would
       * have. What comes back lands in the destination slot, so the frame
       * after this looks exactly as it would have had the interpreter made
       * the call — which is what lets an exit downstream need nothing put
       * back.
       *
       * The callee may throw, and compiled code has no handler. A false
       * answer leaves through the exit reserved for exactly this, and the
       * frame fails with it. */
      const IrCallSite *site = &ir->calls[inst->b];

      /* Nothing is spilled around it. The arguments are in the frame because
       * the slots a call reads are never promoted, and everything else the
       * compiled code holds is in a callee-saved register — which is what
       * `callSafe` restricts the allocator to for a function that calls out. */
      csJitMovRegister(encoder, 0, REG_SLOTS);
      csJitMovImmediate(encoder, 1, (uint64_t)inst->a);
      csJitMovImmediate(encoder, 2, (uint64_t)(uintptr_t)site);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)(uintptr_t)csJitCallClosure);
      csJitWord(encoder, 0xD63F0000u | ((uint32_t)REG_TEMP << 5)); /* blr x9 */

      /* `cbnz w0, over` — anything but false carries on. */
      int over = encoder->count;
      csJitWord(encoder, 0x35000000u); /* cbnz w0, <patched> */

      int failed = csJitAddExit(at, CS_JIT_EXIT_FAILED, 0);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)failed);
      csJitStrWord32(encoder, REG_TEMP, REG_EXIT);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
      csJitWord(encoder, 0xAA0903E0u); /* mov x0, x9 */
      csJitEmitEpilogue(encoder);

      csJitPatchToHere(encoder, over);
      break;
    }

    case IR_NEW_OBJECT: {
      /* The one place compiled code calls out to build something.
       *
       * The values are already in the slots above the destination, put there
       * by the stores the lowering emitted — which is why this needs no list
       * of registers and why those slots are kept out of registers. What is
       * passed is the frame, the destination, and the keys.
       *
       * Everything live across the call is either in a callee-saved register
       * — the allocator is restricted to those for a function that calls out
       * — or in the frame, which is where the collector will look. */
      const IrObjectLiteral *literal = &ir->literals[inst->b];

      csJitMovRegister(encoder, 0, REG_SLOTS);
      csJitMovImmediate(encoder, 1, (uint64_t)inst->a);
      csJitMovImmediate(encoder, 2, (uint64_t)(uintptr_t)literal);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)(uintptr_t)csJitBuildObject);
      csJitWord(encoder, 0xD63F0000u | ((uint32_t)REG_TEMP << 5)); /* blr x9 */
      break;
    }

    case IR_GUARD_SHAPE: {
      /* An exit in the middle of a block, which is what makes it a guard: the
       * check is asked at the read rather than once on the way in, and when it
       * does not hold the interpreter picks the frame up at the property
       * instruction and does the read its own way.
       *
       * The object is read out of the frame by the helper, which is why the
       * guarded slot is kept out of a register, and why a function with a
       * guard in it allocates only from the callee-saved bank. */
      const IrEntryShape *guard = &ir->entryShapes[inst->b];

      csJitMovRegister(encoder, 0, REG_SLOTS);
      csJitMovImmediate(encoder, 1, (uint64_t)(uintptr_t)guard);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)(uintptr_t)csJitShapeHolds);
      csJitWord(encoder, 0xD63F0000u | ((uint32_t)REG_TEMP << 5)); /* blr x9 */

      /* `cbnz w0, over` — the layout held, so carry on. */
      int over = encoder->count;
      csJitWord(encoder, 0x35000000u); /* cbnz w0, <patched> */

      /* It did not. Every promoted slot goes home, exactly as an ordinary exit
       * does it: the interpreter reads locals out of the frame and knows
       * nothing about the registers they have been living in. */
      for (int s = 0; s <= ir->slotCount; s++) {
        if (slotHome[s] >= 0) csJitStrDouble(encoder, slotHome[s], REG_SLOTS, s * 8);
      }

      int leave = csJitAddExit(at, guard->deoptOffset, guard->deoptHeight);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)leave);
      csJitStrWord32(encoder, REG_TEMP, REG_EXIT);
      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
      csJitWord(encoder, 0xAA0903E0u); /* mov x0, x9 */
      csJitEmitEpilogue(encoder);

      csJitPatchToHere(encoder, over);
      break;
    }

    case IR_EXIT: {
      /* Give the frame back. Every promoted slot goes home first: the
       * interpreter reads locals out of the frame and knows nothing about
       * the registers they have been living in. Nothing else needs writing
       * — an exit is only ever emitted at the height its block began at,
       * so every live position is still where the interpreter left it. */
      for (int s = 0; s <= ir->slotCount; s++) {
        if (slotHome[s] >= 0) csJitStrDouble(encoder, slotHome[s], REG_SLOTS, s * 8);
      }

      int exit = csJitAddExit(at, inst->a, inst->b);

      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)exit);
      csJitStrWord32(encoder, REG_TEMP, REG_EXIT);

      csJitMovImmediate(encoder, REG_TEMP, (uint64_t)UNDEFINED_VAL);
      csJitWord(encoder, 0xAA0903E0u); /* mov x0, x9 */
      csJitEmitEpilogue(encoder);
      break;
    }
    /* Deliberate, and the reason it is here rather than left to -Wswitch: a
     * switch with no default emits *nothing* for an instruction it does not
     * know, and nothing is a silent miscompile — the value the instruction was
     * to produce is simply absent, and whatever reads it reads what was there
     * before. Refusing the function is the safe answer to a new opcode, and
     * the warning that would have named it is worth giving up for that. */
    default: {
      *why = "an IR instruction this backend has no encoding for";
      return false;
    }
  }
  if (encoder->failed) {
    *why = "an offset too large to encode";
    return false;
  }
  return true;
#endif
}
