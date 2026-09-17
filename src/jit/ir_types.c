/* ir_types.c — the passes that decide what the lowered form is worth.
 *
 * What each value is known to hold, which of them nothing reads, and whether
 * the arithmetic is proved enough to run. The lowering's own answers are
 * provisional — it walks in linear order and a loop makes linear order the
 * wrong order — so nothing downstream may read a type until these have run.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/debug.h"
#include "cscript/memory.h"
#include "cscript/opcode.h"
#include "cscript/shape.h"
#include "cscript/type.h"
#include "cscript/vm.h"

#include "jit/ir_internal.h"

void csIrRegisterOperands(const IrInst *inst, int *a, int *b) {
  *a = -1;
  *b = -1;

  switch (inst->op) {
    case IR_CONST:
    case IR_JUMP:
    case IR_EXIT:
    /* `a` is a frame slot and `b` a storage index; neither is a register. */
    case IR_LOAD_PROPERTY:
    case IR_LOAD_LOCAL:
    case IR_LOAD_GLOBAL: break; /* neither field is a register */

    case IR_STORE_LOCAL:
    case IR_STORE_GLOBAL:
    /* `a` is the frame slot and `c` the storage index; the value is in `b`,
     * the same place the other two stores keep theirs. */
    case IR_STORE_PROPERTY:
    case IR_ADD_PROPERTY:
      *b = inst->b; /* `a` is the destination, not a value */
      break;

    /* Neither operand is a register: `a` is the destination slot and `b` is an
     * index into the literal table. The values come from the frame. */
    case IR_NEW_OBJECT:
    /* Nor here: `a` is the slot being checked and `b` indexes the shape
     * table. The guard reads the frame, not a register. */
    case IR_GUARD_SHAPE:
    /* Neither operand is a register here either: the arguments are in the
     * frame and `b` indexes the call table. */
    case IR_CALL: break;

    case IR_NEG:
    case IR_RETURN:
    case IR_BRANCH:
      *a = inst->a; /* a branch's `b` and `c` are blocks */
      break;

    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
    case IR_MOD:
    case IR_LT:
    case IR_LE:
    case IR_GT:
    case IR_GE:
    case IR_EQ:
    case IR_NE:
      *a = inst->a;
      *b = inst->b;
      break;
  }
}

void csIrForwardSlots(IrFunction *ir) {
  /* Within a block only. Across one, a slot may be written by another path,
   * and proving it is not needs the dataflow this deliberately does without. */
  int *rename = (int *)malloc(sizeof(int) * (size_t)(ir->registerCount + 1));

  for (int b = 0; b < ir->blockCount; b++) {
    IrBlock *block = &ir->blocks[b];
    for (int r = 0; r < ir->registerCount; r++) rename[r] = r;

    /* slotHolder[s] is the register whose value slot s currently holds, or -1
     * when that is not known. */
    int slotHolder[IR_MAX_STACK];
    for (int s = 0; s < IR_MAX_STACK; s++) slotHolder[s] = -1;

    int kept = 0;
    for (int i = 0; i < block->count; i++) {
      IrInst inst = block->instructions[i];

      /* Operands first: an earlier forward may have renamed them. Only the
       * fields that really are registers — see csIrRegisterOperands. */
      int operandA, operandB;
      csIrRegisterOperands(&inst, &operandA, &operandB);
      if (operandA >= 0 && operandA < ir->registerCount) inst.a = rename[operandA];
      if (operandB >= 0 && operandB < ir->registerCount) inst.b = rename[operandB];

      if (inst.op == IR_LOAD_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK && slotHolder[inst.a] >= 0) {
        /* The value is already in a register: rename and drop the load. */
        rename[inst.result] = slotHolder[inst.a];
        continue;
      }

      if (inst.op == IR_STORE_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK) {
        slotHolder[inst.a] = inst.b;
      } else if (inst.op == IR_LOAD_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK) {
        slotHolder[inst.a] = inst.result;
      } else {
        /* Anything else that writes a slot ends what was believed about it. An
         * allocation is the one that does: forwarding a load past it would
         * answer with whatever register held the slot's *previous* value,
         * which is the shape of a wrong answer that leaves no trace. */
        int written = csIrWritesSlot(&inst);
        if (written >= 0 && written < IR_MAX_STACK) slotHolder[written] = -1;
      }

      block->instructions[kept++] = inst;
    }
    block->count = kept;
  }

  free(rename);
}

void csIrRemoveDeadStores(IrFunction *ir) {
  bool *isRead = (bool *)calloc((size_t)ir->slotCount + 1, sizeof(bool));

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op == IR_LOAD_LOCAL && inst->a >= 0 && inst->a <= ir->slotCount) {
        isRead[inst->a] = true;
      }

      /* A property access reads the slot holding the object, without a load to
       * show for it. Nothing depends on this today — a property is only
       * lowered against a slot the function never writes, so there is no store
       * to remove — but relying on that coincidence is how the next change
       * breaks something quietly. */
      int first;
      int last;
      csIrReadsSlots(ir, inst, &first, &last);
      for (int s = first; s >= 0 && s <= last && s <= ir->slotCount; s++) isRead[s] = true;

      /* An exit reads everything. The interpreter picks the frame up from
       * there and its next instruction may be a load of any live slot — a
       * read this pass cannot see, because it is not in the IR at all.
       * Removing those stores made a loop compute the right answer and then
       * hand back the value it started with. */
      if (inst->op == IR_EXIT) {
        for (int s = 0; s <= ir->slotCount && s < inst->b; s++) isRead[s] = true;
      }

      /* And so does a guard, at the height its record names: it is an exit
       * that happens to be in the middle of a block. */
      if (inst->op == IR_GUARD_SHAPE && inst->b >= 0 && inst->b < ir->entryShapeCount) {
        int height = ir->entryShapes[inst->b].deoptHeight;
        for (int s = 0; s <= ir->slotCount && s < height; s++) isRead[s] = true;
      }
    }
  }

  for (int b = 0; b < ir->blockCount; b++) {
    IrBlock *block = &ir->blocks[b];
    int kept = 0;
    for (int i = 0; i < block->count; i++) {
      const IrInst *inst = &block->instructions[i];
      if (inst->op == IR_STORE_LOCAL && inst->a >= 0 && inst->a <= ir->slotCount && !isRead[inst->a]) {
        continue;
      }
      block->instructions[kept++] = block->instructions[i];
    }
    block->count = kept;
  }

  free(isRead);
}

/* ---- typing the slots ---------------------------------------------------
 *
 * One type per slot for the whole function is the coarse answer, and it is
 * wrong in a way that costs. The operand stack and the locals are the same
 * array here, so two loops in one file count in the same frame position — and
 * a `while (true)` that puts a boolean there leaves every other loop's counter
 * with no type at all, which refuses every comparison that reads it.
 *
 * The precise answer is a meet over each block's predecessors, iterated to a
 * fixed point. That needs no SSA and no dominance, only the predecessors the
 * jumps already name — plus one edge they do not: the lowering's own walk in
 * bytecode order, which is a real path into a block and the only one on the
 * way into a loop the compiler takes over part-way through.
 *
 * Where that walk cannot be believed — the lowering skipped a run it could not
 * replay, so the linear state describes a path that did not happen — the
 * whole-function meet is what is left, and is what this falls back to.
 */

/* A slot with no constraint on it yet, which is not the same as a slot nothing
 * is known about: the first contribution replaces it, where meeting with
 * IR_TYPE_UNKNOWN would swallow everything. */
#define SLOT_TOP (-1)

static int meetSlot(int a, int b) {
  if (a == SLOT_TOP) return b;
  if (b == SLOT_TOP) return a;
  return a == b ? a : (int)IR_TYPE_UNKNOWN;
}

/* The lowering cuts a block at its first terminator, and a block that has none
 * runs into the next one — which the code generator relies on, laying the
 * blocks out in order. So linear succession is a real edge. */
static bool blockFallsThrough(const IrBlock *block) {
  if (block->count == 0) return true;
  IrOp last = block->instructions[block->count - 1].op;
  return last != IR_JUMP && last != IR_BRANCH && last != IR_RETURN && last != IR_EXIT;
}

/* Applies a block to a slot state, and where asked retypes the loads it makes
 * on the way through. Returns whether retyping changed anything. */
static bool walkBlockSlots(IrFunction *ir, int b, int *state, int slots, bool retype) {
  IrBlock *block = &ir->blocks[b];
  bool changed = false;

  for (int i = 0; i < block->count; i++) {
    IrInst *inst = &block->instructions[i];

    if (inst->op == IR_LOAD_LOCAL && inst->a >= 0 && inst->a < slots) {
      if (!retype) continue;
      int known = state[inst->a];
      IrType type = known == SLOT_TOP ? IR_TYPE_UNKNOWN : (IrType)known;
      if (inst->type == type) continue;
      inst->type = type;
      if (inst->result >= 0 && inst->result <= ir->registerCount) {
        ir->registerTypes[inst->result] = type;
      }
      changed = true;
      continue;
    }

    if (inst->op == IR_STORE_LOCAL && inst->a >= 0 && inst->a < slots && inst->b >= 0 && inst->b <= ir->registerCount) {
      state[inst->a] = (int)ir->registerTypes[inst->b];
      continue;
    }

    /* A slot an allocation writes holds an object; one a call writes holds
     * whatever the callee declared it answers. */
    int written = csIrWritesSlot(inst);
    if (written >= 0 && written < slots) state[written] = (int)csIrWrittenType(ir, inst);
  }
  return changed;
}

/* The meet of everything stored into each slot, anywhere in the function. The
 * coarse answer, still needed twice: as the fallback when the per-block one
 * cannot be trusted, and as what the register allocator asks when it decides
 * whether a slot can live in a register for the whole run. */
static void wholeFunctionMeet(const IrFunction *ir, IrType *meet, int slots) {
  bool written[IR_MAX_SLOTS];
  for (int s = 0; s < slots; s++) {
    meet[s] = IR_TYPE_UNKNOWN;
    written[s] = false;
  }

  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (csIrWritesSlot(inst) < 0) continue;
      if (inst->a < 0 || inst->a >= slots) continue;
      if (inst->op == IR_STORE_LOCAL && (inst->b < 0 || inst->b > ir->registerCount)) continue;

      /* What an allocation leaves in the slot is an object: not a number, and
       * so not a slot that may be promoted into a floating-point register. */
      IrType stored = csIrWrittenType(ir, inst);
      if (!written[inst->a]) {
        meet[inst->a] = stored;
        written[inst->a] = true;
      } else if (meet[inst->a] != stored) {
        meet[inst->a] = IR_TYPE_UNKNOWN;
      }
    }
  }

  /* A slot nothing stores to holds whatever the caller or the interpreter left
   * there, which is only known for an annotated parameter. */
  for (int s = 0; s < slots; s++) {
    if (!written[s]) meet[s] = ir->slotTypes[s];
  }
}

void csIrReconcileSlotTypes(IrFunction *ir) {
  int slots = ir->slotCount + 1;
  if (slots > IR_MAX_SLOTS) slots = IR_MAX_SLOTS;
  if (slots < 1) slots = 1;
  int blocks = ir->blockCount;

  IrType *meet = (IrType *)malloc(sizeof(IrType) * (size_t)slots);

  /* Every edge the block graph has, built once. Two per block at most — a
   * branch's two arms — plus the fall-through into the next. */
  int edgeCapacity = blocks * 3 + 1;
  int *edgeFrom = (int *)malloc(sizeof(int) * (size_t)edgeCapacity);
  int *edgeTo = (int *)malloc(sizeof(int) * (size_t)edgeCapacity);
  int edgeCount = 0;
  for (int b = 0; b < blocks; b++) {
    int targets[3] = {-1, -1, -1};
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      if (inst->op == IR_JUMP) targets[0] = inst->a;
      if (inst->op == IR_BRANCH) {
        targets[0] = inst->b;
        targets[1] = inst->c;
      }
    }
    if (blockFallsThrough(&ir->blocks[b])) targets[2] = b + 1;

    for (int k = 0; k < 3; k++) {
      if (targets[k] < 0 || targets[k] >= blocks) continue;
      if (edgeCount >= edgeCapacity) break;
      edgeFrom[edgeCount] = b;
      edgeTo[edgeCount] = targets[k];
      edgeCount++;
    }
  }

  int *entry = (int *)malloc(sizeof(int) * (size_t)blocks * (size_t)slots);
  int *exit = (int *)malloc(sizeof(int) * (size_t)blocks * (size_t)slots);
  int *state = (int *)malloc(sizeof(int) * (size_t)slots);
  bool *have = (bool *)malloc(sizeof(bool) * (size_t)blocks);

  bool perBlock = ir->blockEntryTrusted && ir->blockEntryTypes != NULL && ir->blockEntrySeeded != NULL && blocks > 0;

  /* Iterated because retyping a load retypes what is computed from it, which
   * can retype the slot the result is stored to in turn. It settles quickly:
   * every round can only remove information. */
  for (int round = 0; round < 8; round++) {
    wholeFunctionMeet(ir, meet, slots);

    bool changed = false;
    if (!perBlock) {
      /* The coarse answer applied everywhere, which is what this pass did
       * before there was a finer one. */
      for (int b = 0; b < blocks; b++) {
        for (int s = 0; s < slots; s++) state[s] = (int)meet[s];
        if (walkBlockSlots(ir, b, state, slots, true)) changed = true;
      }
    } else {
      for (int i = 0; i < blocks * slots; i++) exit[i] = SLOT_TOP;

      /* Chaotic iteration to a fixed point. The lattice is two deep, so a
       * handful of sweeps settles it; the bound is there to terminate rather
       * than to be reached. */
      for (int sweep = 0; sweep < blocks + 4; sweep++) {
        bool grew = false;

        for (int b = 0; b < blocks; b++) {
          have[b] = ir->blockEntrySeeded[b];
          for (int s = 0; s < slots; s++) {
            entry[b * slots + s] = have[b] ? (int)ir->blockEntryTypes[(size_t)b * IR_MAX_SLOTS + s] : SLOT_TOP;
          }
        }
        for (int e = 0; e < edgeCount; e++) {
          int from = edgeFrom[e], to = edgeTo[e];
          for (int s = 0; s < slots; s++) {
            entry[to * slots + s] = meetSlot(entry[to * slots + s], exit[from * slots + s]);
          }
          have[to] = true;
        }

        for (int b = 0; b < blocks; b++) {
          for (int s = 0; s < slots; s++) state[s] = entry[b * slots + s];
          walkBlockSlots(ir, b, state, slots, false);
          for (int s = 0; s < slots; s++) {
            if (exit[b * slots + s] == state[s]) continue;
            exit[b * slots + s] = state[s];
            grew = true;
          }
        }
        if (!grew) break;
      }

      /* And now the loads, from the state at their own position rather than
       * from a verdict about the whole function. A block nothing reaches and
       * nothing recorded a state for keeps the coarse answer: it is one the
       * lowering stopped short of, so it holds no loads that matter, and
       * guessing would be the one thing that is not safe. */
      for (int b = 0; b < blocks; b++) {
        for (int s = 0; s < slots; s++) {
          state[s] = have[b] ? entry[b * slots + s] : (int)meet[s];
        }
        if (walkBlockSlots(ir, b, state, slots, true)) changed = true;
      }
    }

    /* The register allocator asks whether a slot can live in a register for
     * the whole run, which is a whole-function question however precisely the
     * loads are typed. */
    wholeFunctionMeet(ir, meet, slots);
    for (int s = 0; s < slots; s++) ir->slotTypes[s] = meet[s];
    if (!changed) break;
  }

  free(meet);
  free(edgeFrom);
  free(edgeTo);
  free(entry);
  free(exit);
  free(state);
  free(have);
}

bool csIrIsFullyTyped(const IrFunction *ir) {
  /* The question is not whether every value is typed — the compiler appends an
   * unreachable `return undefined` to every function, and a blanket check
   * fails on that alone. It is whether every value that gets *arithmetic done
   * to it* is known to be a number. Those are the operations a code generator
   * would emit unboxed, and the only ones that need a guard without a proof.
   *
   * Equality is excluded on purpose: it is defined for every type and needs no
   * proof about its operands. */
  /* And whether there is anything here worth emitting at all. Arithmetic is
   * the obvious answer, and a property touched is the other one: a constructor
   * does no arithmetic and is two stores, which is exactly the shape this
   * used to refuse for having nothing in it. */
  bool sawWork = false;
  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      switch (inst->op) {
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
        case IR_LT:
        case IR_LE:
        case IR_GT:
        case IR_GE:
          if (ir->registerTypes[inst->a] != IR_TYPE_NUMBER) return false;
          if (ir->registerTypes[inst->b] != IR_TYPE_NUMBER) return false;
          sawWork = true;
          break;
        case IR_NEG:
          if (ir->registerTypes[inst->a] != IR_TYPE_NUMBER) return false;
          sawWork = true;
          break;

        /* A store is work: what it replaces is a shape compare, a cache read
         * and an indexed store in the interpreter's loop. */
        case IR_STORE_PROPERTY:
        case IR_ADD_PROPERTY:
        case IR_LOAD_PROPERTY: sawWork = true; break;

        case IR_BRANCH:
          /* A branch has to be on something the IR can test. */
          if (ir->registerTypes[inst->a] != IR_TYPE_BOOL) return false;
          break;
        default: break;
      }
    }
  }
  return sawWork;
}
