/* ir.c — the walk that lowers a function's bytecode to the typed IR.
 *
 * The lowering is an abstract interpretation of the bytecode: it walks the
 * instructions keeping a compile-time model of the operand stack, where each
 * entry is a virtual register rather than a value. An OP_ADD pops two register
 * names and pushes a third — which is exactly the translation from a stack
 * machine to three-address code.
 *
 * Control flow is found in one pass first, because a block boundary is wherever
 * a jump lands, and that is only known after the whole chunk has been read.
 *
 * What each instruction lowers to is in the four ir_lower_*.c files, asked in
 * turn; what they share is in ir_internal.h. This file is the loop around
 * them, and the two things only it can do: hand the frame back to the
 * interpreter, and decide what to say about the blocks nothing reached.
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

IrFunction *csIrLower(ObjFunction *function, const char **reason) {
  *reason = NULL;
  const Chunk *chunk = &function->chunk;

  if (chunk->count >= IR_MAX_BLOCKS * 8) {
    *reason = "function is too large";
    return NULL;
  }

  bool *leader = (bool *)calloc((size_t)chunk->count + 1, sizeof(bool));
  if (!csIrMarkLeaders(chunk, leader, reason)) {
    free(leader);
    return NULL;
  }

  IrFunction *ir = (IrFunction *)calloc(1, sizeof(IrFunction));
  ir->source = function;
  ir->slotCount = function->arity + 1; /* slot 0 is the callee or receiver */

  /* One block per leader, in bytecode order, so a jump can be resolved to an
   * index without a second pass over the blocks. */
  for (int offset = 0; offset < chunk->count; offset++) {
    if (!leader[offset]) continue;
    if (ir->blockCapacity < ir->blockCount + 1) {
      ir->blockCapacity = ir->blockCapacity < 8 ? 8 : ir->blockCapacity * 2;
      ir->blocks = (IrBlock *)realloc(ir->blocks, sizeof(IrBlock) * (size_t)ir->blockCapacity);
    }
    memset(&ir->blocks[ir->blockCount], 0, sizeof(IrBlock));
    ir->blocks[ir->blockCount].bytecodeStart = offset;
    ir->blockCount++;
  }

  ir->blockEntryTypes = (IrType *)calloc((size_t)ir->blockCount * IR_MAX_SLOTS + 1, sizeof(IrType));
  ir->blockEntrySeeded = (bool *)calloc((size_t)ir->blockCount + 1, sizeof(bool));
  ir->blockEntryTrusted = true;

  Lowering low;
  memset(&low, 0, sizeof low);
  low.ir = ir;
  low.chunk = chunk;
  low.reason = NULL;

  /* A frame arrives with slot 0 holding the callee and the arguments above it,
   * so the first free position — where a temporary goes — is arity + 1.
   * Starting at zero would have the first push overwrite a parameter. */
  low.stackTop = function->arity + 1;
  for (int i = 0; i < low.stackTop; i++) low.stack[i] = -1;
  for (int i = 0; i < IR_MAX_BLOCKS; i++) low.entryHeight[i] = -1;
  for (int i = 0; i < IR_MAX_STACK; i++) low.slotType[i] = IR_TYPE_UNKNOWN;

  /* This is where an annotation stops being advice and starts being usable:
   * a parameter the checker proved is a number becomes a slot the lowering
   * knows holds one, and every read of it is typed.
   *
   * An observation counts too, and differently. An annotation is a promise the
   * checker enforces at every call; an observation is only what has happened
   * so far, so code built on one is guarded at entry and refused when the
   * guess stops holding. Without this almost nothing is compilable: an
   * unannotated parameter makes every arithmetic site that touches it
   * untyped, and most parameters are unannotated. */
  for (int i = 0; i < function->arity && i + 1 < IR_MAX_STACK; i++) {
    bool annotated = function->paramTypes != NULL && function->paramTypes[i] == TYPE_NUMBER;
    bool observed = function->observedParams != NULL && function->observedParams[i] == CS_PARAM_NUMBER;
    if (annotated || observed) low.slotType[i + 1] = IR_TYPE_NUMBER;
  }

  /* Set once a region has been skipped. After that the linear stack height is
   * meaningless — the skipped code pushed and popped who knows what — so a
   * block is only lowered when a jump recorded the height it is entered at. */
  bool skipped = false;

  int blockIndex = -1;
  /* The lowest the operand stack has been since this block started.
   *
   * Every position the block has written is at or above that mark — to write
   * position p the stack has to have been p deep — so everything below it is
   * still exactly where the interpreter left it. That is what makes an exit
   * safe, and a pop harmless: it is pushing that puts a live value somewhere
   * the interpreter cannot see. */
  int blockFloor = low.stackTop;
  /* And where that was, in bytecode and in emitted instructions.
   *
   * An exit has to happen at the floor, but the thing that forces one — a call,
   * a string concatenation, an opcode with no IR form — is usually found with
   * its operands already pushed. Rewinding to the last point the stack was at
   * the floor, and throwing away what was emitted since, puts the exit where
   * it can go: the interpreter simply redoes that statement from the start. */
  int floorOffset = 0;
  int floorCount = 0;
  for (int offset = 0; offset < chunk->count;) {
    if (leader[offset]) {
      blockIndex = csIrBlockAt(ir, offset);
      /* A recorded height beats the linear one: it came from an actual jump,
       * or from the taken arm of one inside a run handed over.
       *
       * It is also a *fact*, which is what lets the walk pick up again: after
       * a run it could not replay the linear height means nothing, but a
       * recorded one means something, and everything downstream of here needs
       * a height more than it needs types. What the slots hold is the part
       * that is not known, so where no predecessor recorded it, nothing is
       * claimed — the alternative is carrying a stale belief forward and
       * recording it as though it were a path that happened. */
      if (blockIndex >= 0 && blockIndex < IR_MAX_BLOCKS && low.entryHeight[blockIndex] >= 0) {
        low.stackTop = low.entryHeight[blockIndex];
        if (skipped) {
          if (blockIndex < ir->blockCount && ir->blockEntrySeeded[blockIndex]) {
            memcpy(low.slotType, &ir->blockEntryTypes[(size_t)blockIndex * IR_MAX_SLOTS], sizeof low.slotType);
          } else {
            for (int s = 0; s < IR_MAX_STACK; s++) low.slotType[s] = IR_TYPE_UNKNOWN;
          }
          for (int s = 0; s < IR_MAX_STACK; s++) low.stack[s] = -1;
          skipped = false;
        }
      } else if (skipped) {
        /* Nothing reaches this block from the part being compiled, and its
         * height is unknown, so the whole of it is left to the interpreter —
         * see blockEntryTrusted for why nothing recorded past here counts.
         * up to the next block, which may have a height a jump recorded. */
        int skip = csInstructionLength(chunk, offset);
        if (skip <= offset) {
          low.reason = "could not be decoded";
          goto failed;
        }
        while (skip < chunk->count && !leader[skip]) {
          int step = csInstructionLength(chunk, skip);
          if (step <= skip) {
            low.reason = "could not be decoded";
            goto failed;
          }
          skip = step;
        }
        offset = skip;
        continue;
      }
      /* Nothing the linear walk knows about registers survives a block
       * boundary. A leader can be reached by a jump as well as by falling
       * into it, and the two paths leave different registers holding the same
       * stack position — so every entry is forgotten here, and whatever needs
       * a value loads it from its slot instead. The sites that read this array
       * all refuse a forgotten entry rather than guessing, which makes the
       * cost a function left uncompiled rather than one compiled wrongly. */
      for (int s = 0; s < IR_MAX_STACK; s++) low.stack[s] = -1;

      /* Nothing the linear walk knows about registers survives a block
       * boundary. A leader can be reached by a jump as well as by falling
       * into it, and the two paths leave different registers holding the same
       * stack position — so every entry is forgotten here, and whatever needs
       * a value loads it from its slot instead. The sites that read this array
       * all refuse a forgotten entry rather than guessing, which makes the
       * cost a function left uncompiled rather than one compiled wrongly. */
      for (int s = 0; s < IR_MAX_STACK; s++) low.stack[s] = -1;

      blockFloor = low.stackTop;
      floorOffset = offset;
      floorCount = 0;
      /* A callee placeholder never outlives the run of instructions it was
       * pushed in — callSiteFor refuses a call another path can reach — so
       * there is nothing here to clear. Clearing anyway is what keeps that
       * true if the search ever widens. */
      csIrClearPendingCallees(&low);

      /* What the lowering walked into this block believing. That is a real
       * path — the one the interpreter also takes to get here — and it is the
       * only one on the way into a loop the compiler takes over part-way
       * through, where no lowered jump reaches the header at all.
       *
       * Merged with anything a skipped jump's taken arm already recorded for
       * this block, because that is a real path too; and where the walk itself
       * has nothing to say, the recorded state is adopted so that the walk has
       * something true to carry on from. */
      if (blockIndex >= 0 && blockIndex < ir->blockCount) {
        IrType *recorded = &ir->blockEntryTypes[(size_t)blockIndex * IR_MAX_SLOTS];
        if (!ir->blockEntrySeeded[blockIndex]) {
          memcpy(recorded, low.slotType, sizeof low.slotType);
          ir->blockEntrySeeded[blockIndex] = true;
        } else {
          for (int s = 0; s < IR_MAX_SLOTS; s++) {
            if (recorded[s] != low.slotType[s]) recorded[s] = IR_TYPE_UNKNOWN;
          }
          memcpy(low.slotType, recorded, sizeof low.slotType);
        }
      }
    }

    /* The abstract stack is *not* reset at a block boundary.
     *
     * In this VM a local is a stack slot: `let x = 1` leaves its value on the
     * stack and calls that position a local, emitting no instruction at all.
     * So the operand stack and the locals are the same array, and clearing it
     * between blocks would lose every local in scope. Carrying the height
     * along linear order is right for the code this compiler emits, because it
     * is structured — and where it is not, the lowering underflows and refuses
     * rather than producing something subtly wrong. */
    IrBlock *block = &ir->blocks[blockIndex];
    if (low.stackTop < blockFloor) blockFloor = low.stackTop;
    if (low.stackTop == blockFloor) {
      floorOffset = offset;
      floorCount = block->count;
    }

    uint8_t opcode = chunk->code[offset];
    int line = chunk->lines[offset];
    int next = csInstructionLength(chunk, offset);
    int jumpTarget = next + ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);

    /* Arithmetic on something not known to be a number would need a guard, and
     * a guard is the one thing this compiler is built not to emit. Rather than
     * lower it and have the whole function refused for it — which is what a
     * string concatenation after a numeric loop used to do — the frame goes
     * back to the interpreter at that statement.
     *
     * Equality is not here: it is defined for every type and needs no proof
     * about its operands. */
    /* Every slot the lowering models has to fit the arrays that model it. The
     * operand is one byte, so it reaches 255, and the model is IR_MAX_SLOTS
     * wide — an unchecked one read past the end of a stack array, which is
     * undefined behaviour that happened to return a plausible type. */
    switch (opcode) {
      case OP_GET_LOCAL:
      case OP_SET_LOCAL:
      case OP_SET_LOCAL_POP:
      case OP_INC_LOCAL:
      case OP_DEC_LOCAL:
      case OP_GET_LOCAL_CONST:
      case OP_GET_LOCAL_PROPERTY:
        if (chunk->code[offset + 1] >= IR_MAX_SLOTS) goto handOver;
        break;
      case OP_GET_LOCAL_LOCAL:
        if (chunk->code[offset + 1] >= IR_MAX_SLOTS || chunk->code[offset + 2] >= IR_MAX_SLOTS) {
          goto handOver;
        }
        break;
      default: break;
    }

    int wantsNumbers = 0;
    switch (opcode) {
      case OP_NEGATE: wantsNumbers = 1; break;
      case OP_ADD:
      case OP_SUBTRACT:
      case OP_MULTIPLY:
      case OP_DIVIDE:
      case OP_MODULO:
      case OP_LESS:
      case OP_LESS_EQUAL:
      case OP_GREATER:
      case OP_GREATER_EQUAL: wantsNumbers = 2; break;
      default: break;
    }
    for (int k = 0; k < wantsNumbers; k++) {
      if (low.stackTop - 1 - k < 0) {
        low.reason = "operand stack underflow while lowering";
        goto failed;
      }
      /* A position the interpreter filled holds no register of ours — see
       * the replay in handOver — so there is nothing here to prove numeric. */
      int operand = low.stack[low.stackTop - 1 - k];
      if (operand < 0 || ir->registerTypes[operand] != IR_TYPE_NUMBER) {
        goto handOver;
      }
    }

    /* Asked of each group in turn. Every one answers for the opcodes it was
     * written for and LOWER_UNHANDLED for the rest, so what happens to an
     * opcode none of them claims is the hand-over below — which is what the
     * switch's `default` did when they were all one function. */
    LowerAt at = {&low, ir, block, chunk, function, leader, offset, next, line, jumpTarget};
    LowerResult lowered = csIrLowerData(&at);
    if (lowered == LOWER_UNHANDLED) lowered = csIrLowerArith(&at);
    if (lowered == LOWER_UNHANDLED) lowered = csIrLowerObject(&at);
    if (lowered == LOWER_UNHANDLED) lowered = csIrLowerFlow(&at);

    if (lowered == LOWER_FAILED) goto failed;
    if (lowered != LOWER_OK) goto handOver;

    offset = next;
    continue;

  handOver: {
    /* Something this form cannot express, or arithmetic it cannot prove.
     * Rather than refuse the whole function, the frame goes back to the
     * interpreter here — at the last point the operand stack was at this
     * block's floor, because a value pushed since then lives in a register
     * the interpreter has no name for. Everything emitted since is dropped:
     * none of it ran, and the interpreter redoes that statement from its
     * beginning. */
    if (blockFloor < 0 || blockFloor >= IR_MAX_STACK || floorCount > block->count) {
      low.reason = csOpcodeName((OpCode)opcode);
      goto failed;
    }
    block->count = floorCount;
    csIrClearPendingCallees(&low);

    IrInst *exit = csIrAppend(block, IR_EXIT, line);
    exit->a = floorOffset;
    exit->b = blockFloor;
    ir->hasExits = true;
    /* Remembered so the tiering report can say what the compiler gave up on
     * rather than only that it did. The first one is the interesting one:
     * everything after it is downstream of the same gap. */
    if (ir->firstExitOn == NULL) ir->firstExitOn = csOpcodeName((OpCode)opcode);

    /* Everything up to the next jump target is the interpreter's now.
     * Blocks past it are still lowered: a loop whose body this compiler
     * understands is usually followed by code it does not. */
    int skip = next;
    while (skip < chunk->count && !leader[skip]) {
      int step = csInstructionLength(chunk, skip);
      if (step <= skip) {
        low.reason = "could not be decoded";
        goto failed;
      }
      skip = step;
    }

    /* Where that leaves the frame is not always a mystery. The interpreter
     * picks it up at the exit's offset, not at the instruction that forced
     * one, so the run to replay starts at the floor — and when every
     * instruction in it has a fixed effect, the height and the slot types at
     * the other end follow from the bytecode rather than being unknown. That
     * is what lets a loop below a function declaration still compile. */
    ReplayJump taken;
    const char *replayRefusal = NULL;
    int resumed = csIrReplayHandedOver(chunk, low.slotType, floorOffset, skip, blockFloor, &taken, &replayRefusal);

    /* A run that ends in a jump has a second arm, and the block it lands on
     * has to know about it or every entry type derived for that block comes
     * from the wrong set of paths. Recording it is what lets a run with a
     * `break` in it be replayed at all. */
    if (resumed >= 0 && taken.target >= 0 && !csIrRecordArrival(ir, &low, csIrBlockAt(ir, taken.target), taken.slotType, taken.height)) {
      replayRefusal = "a jump whose arms disagree on the stack height";
      resumed = -1;
    }

    if (resumed >= 0) {
      low.stackTop = resumed;
      /* Nothing this function computed is in those positions any more: the
       * interpreter put the values there. Marking them as holding no
       * register is what stops a later instruction reading a stale one. */
      for (int s = 0; s < IR_MAX_STACK; s++) low.stack[s] = -1;
      /* An unconditional jump does not reach the end of the run, so the
       * height there is not a fact about anything. The block that starts
       * there is entered by its own predecessors or not at all, and the walk
       * picks up again wherever one of them recorded a state. */
      if (!taken.fallsThrough) skipped = true;
    } else {
      /* From here the linear state describes a path that did not happen, so
       * nothing recorded after this point can be believed — and the seeds
       * already taken are only sound while every one of them is. */
      skipped = true;
      ir->blockEntryTrusted = false;
      if (ir->firstReplayRefusal == NULL) ir->firstReplayRefusal = replayRefusal;
    }

    offset = skip;
    continue;
  }
  }

  /* The slot types the lowering settled on, kept for the allocator. */
  ir->slotTypes = (IrType *)malloc(sizeof(IrType) * (size_t)(ir->slotCount + 1));
  for (int s = 0; s <= ir->slotCount; s++) {
    ir->slotTypes[s] = s < IR_MAX_STACK ? low.slotType[s] : IR_TYPE_UNKNOWN;
  }

  /* Nothing after a block's terminator can run: a jump target starts a block
   * of its own, so there is no way in. Cutting it is not tidying — the
   * compiler appends an unreachable `return undefined` to every function, and
   * the store that sets up its value made the slot it used look as though it
   * held two different types. Every pass downstream reads types, so the dead
   * tail has to go before any of them look. */
  for (int b = 0; b < ir->blockCount; b++) {
    IrBlock *block = &ir->blocks[b];
    for (int i = 0; i < block->count; i++) {
      IrOp op = block->instructions[i].op;
      if (op != IR_RETURN && op != IR_EXIT && op != IR_JUMP && op != IR_BRANCH) {
        continue;
      }
      block->count = i + 1;
      break;
    }
  }

  /* Any block left empty is one the lowering stopped short of. It still needs
   * a terminator, and the only right one is to hand the frame over at the
   * offset it stands for — at the height the jump that reaches it recorded,
   * which is precisely what makes that safe.
   *
   * A block with no recorded height is one no lowered jump reaches, so no
   * height would be right and none is invented: the reachability walk below
   * proves the compiled code can never arrive there, and refuses the whole
   * function if that proof fails. Guessing zero here truncated the operand
   * stack and corrupted the frame, which is how this check came to exist. */
  bool *guessed = (bool *)calloc((size_t)ir->blockCount + 1, sizeof(bool));
  for (int b = 0; b < ir->blockCount; b++) {
    if (ir->blocks[b].count > 0) continue;
    int height = b < IR_MAX_BLOCKS ? low.entryHeight[b] : -1;
    guessed[b] = height < 0;

    IrInst *exit = csIrAppend(&ir->blocks[b], IR_EXIT, 0);
    exit->a = ir->blocks[b].bytecodeStart;
    exit->b = height < 0 ? 0 : height;
    ir->hasExits = true;
  }

  bool *reachable = (bool *)calloc((size_t)ir->blockCount + 1, sizeof(bool));
  reachable[0] = true;
  for (bool grew = true; grew;) {
    grew = false;
    for (int b = 0; b < ir->blockCount; b++) {
      if (!reachable[b]) continue;

      int targets[3] = {-1, -1, -1};
      for (int i = 0; i < ir->blocks[b].count; i++) {
        const IrInst *inst = &ir->blocks[b].instructions[i];
        if (inst->op == IR_JUMP) targets[0] = inst->a;
        if (inst->op == IR_BRANCH) {
          targets[0] = inst->b;
          targets[1] = inst->c;
        }
      }
      /* And the edge no instruction names. A block the lowering cut short of a
       * terminator runs into the next one, which the code generator relies on
       * when it lays them out in order — so leaving it out of this walk let a
       * block with a fabricated entry height be declared unreachable and then
       * be reached, handing the interpreter a frame at the wrong depth. */
      int count = ir->blocks[b].count;
      if (count == 0) {
        targets[2] = b + 1;
      } else {
        IrOp last = ir->blocks[b].instructions[count - 1].op;
        if (last != IR_JUMP && last != IR_BRANCH && last != IR_RETURN && last != IR_EXIT) {
          targets[2] = b + 1;
        }
      }

      for (int k = 0; k < 3; k++) {
        int to = targets[k];
        if (to < 0 || to >= ir->blockCount || reachable[to]) continue;
        reachable[to] = true;
        grew = true;
      }
    }
  }

  for (int b = 0; b < ir->blockCount; b++) {
    if (reachable[b] && guessed[b]) {
      free(guessed);
      free(reachable);
      low.reason = "a reachable block whose operand-stack height is unknown";
      goto failed;
    }
  }
  free(guessed);
  free(reachable);

  free(leader);
  return ir;

failed:
  *reason = low.reason != NULL ? low.reason : "unsupported";
  free(leader);
  csIrFree(ir);
  return NULL;
}

void csIrFree(IrFunction *ir) {
  if (ir == NULL) return;
  for (int i = 0; i < ir->blockCount; i++) free(ir->blocks[i].instructions);
  free(ir->blocks);
  free(ir->registerTypes);
  free(ir->slotTypes);
  free(ir->entryShapes);
  free(ir->inlined);
  free(ir->blockEntryTypes);
  free(ir->blockEntrySeeded);
  free(ir);
}
