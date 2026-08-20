/* ir.c — lowering bytecode to the typed IR, printing it, and running it.
 *
 * The lowering is an abstract interpretation of the bytecode: it walks the
 * instructions keeping a compile-time model of the operand stack, where each
 * entry is a virtual register rather than a value. An OP_ADD pops two register
 * names and pushes a third — which is exactly the translation from a stack
 * machine to three-address code.
 *
 * Control flow is found in one pass first, because a block boundary is wherever
 * a jump lands, and that is only known after the whole chunk has been read.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/debug.h"
#include "cscript/ir.h"
#include "cscript/type.h"
#include "cscript/opcode.h"

#define IR_MAX_STACK 64
#define IR_MAX_BLOCKS 256

/* ---- building ---------------------------------------------------------- */

static int newRegister(IrFunction *ir, IrType type) {
  if (ir->registerCapacity < ir->registerCount + 1) {
    ir->registerCapacity = ir->registerCapacity < 16 ? 16 : ir->registerCapacity * 2;
    ir->registerTypes =
        (IrType *)realloc(ir->registerTypes, sizeof(IrType) * (size_t)ir->registerCapacity);
  }
  ir->registerTypes[ir->registerCount] = type;
  return ir->registerCount++;
}

static IrInst *append(IrBlock *block, IrOp op, int line) {
  if (block->capacity < block->count + 1) {
    block->capacity = block->capacity < 8 ? 8 : block->capacity * 2;
    block->instructions =
        (IrInst *)realloc(block->instructions, sizeof(IrInst) * (size_t)block->capacity);
  }
  IrInst *inst = &block->instructions[block->count++];
  memset(inst, 0, sizeof *inst);
  inst->op = op;
  inst->result = -1;
  inst->a = inst->b = inst->c = -1;
  inst->type = IR_TYPE_UNKNOWN;
  inst->line = line;
  return inst;
}

/* ---- finding the block boundaries -------------------------------------- */

/* Every jump target starts a block, and so does the instruction after a jump.
 * Both are found by reading the chunk once; the lowering then walks it again
 * knowing where the seams are. */
static bool markLeaders(const Chunk *chunk, bool *leader, const char **reason) {
  leader[0] = true;

  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    int next = csInstructionLength(chunk, offset);
    if (next <= offset) {
      *reason = "could not be decoded";
      return false;
    }

    switch (opcode) {
      case OP_JUMP:
      case OP_JUMP_IF_FALSE:
      case OP_JUMP_IF_TRUE:
      case OP_POP_JUMP_IF_FALSE:
      case OP_JUMP_IF_NOT_LESS:
      case OP_JUMP_IF_NOT_LESS_EQUAL:
      case OP_JUMP_IF_NOT_GREATER:
      case OP_JUMP_IF_NOT_GREATER_EQUAL:
      case OP_JUMP_IF_NOT_EQUAL:
      case OP_JUMP_IF_EQUAL: {
        int target = next + ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        if (target < chunk->count) leader[target] = true;
        if (next < chunk->count) leader[next] = true;
        break;
      }
      case OP_LOOP: {
        int target = next - ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        if (target >= 0) leader[target] = true;
        if (next < chunk->count) leader[next] = true;
        break;
      }
      default: break;
    }
    offset = next;
  }
  return true;
}

/* Whether the module already has a number under that name.
 *
 * Read at the moment the function turns hot, which is the only moment the
 * lowering has a running program to ask. What keeps the answer true
 * afterwards is checked again at every entry — see the guards on JitCode. */
static bool globalHoldsNumber(const ObjFunction *function, const Chunk *chunk,
                              int nameIndex) {
  if (function->module == NULL) return false;
  if (nameIndex < 0 || nameIndex >= chunk->constants.count) return false;

  Value name = chunk->constants.values[nameIndex];
  if (!IS_STRING(name)) return false;

  Value current;
  if (!csTableGet(&function->module->globals, AS_STRING(name), &current)) return false;
  return IS_NUMBER(current);
}

/* ---- lowering ---------------------------------------------------------- */

typedef struct {
  IrFunction *ir;
  const Chunk *chunk;
  int stack[IR_MAX_STACK]; /* virtual register per operand-stack slot */
  int stackTop;
  int blockOf[IR_MAX_BLOCKS]; /* bytecode offset -> block index */

  /* The operand-stack height each block is entered with.
   *
   * Linear order is not enough to know it: a block reached by a jump inherits
   * the height at the *jump*, which may differ from the height the preceding
   * instruction happened to leave. Recording it when the jump is emitted, and
   * adopting it on arrival, is what keeps the model and the real frame in
   * agreement — and a disagreement between two predecessors is refused rather
   * than guessed at. */
  int entryHeight[IR_MAX_BLOCKS];

  /* What is known about each frame slot. Seeded from the declared parameter
   * types and updated as stores go by; a slot written with two different types
   * falls back to unknown and stays there, which is the conservative answer
   * and costs only a missed specialisation. */
  IrType slotType[IR_MAX_STACK];
  const char *reason;
} Lowering;

/* Notes that `target` is entered with `height` on the stack. */
static bool reachBlock(Lowering *low, int target, int height) {
  if (target < 0 || target >= IR_MAX_BLOCKS) return true;
  if (low->entryHeight[target] < 0) {
    low->entryHeight[target] = height;
    return true;
  }
  if (low->entryHeight[target] != height) {
    low->reason = "operand stack height differs between paths into a block";
    return false;
  }
  return true;
}

/* The operand stack and the locals are the same array.
 *
 * `let total = 0` emits no instruction at all: the initialiser leaves its value
 * on the stack and the compiler simply calls that position a local from then
 * on. So a model with separate slots and temporaries is wrong — it loses every
 * local that was declared rather than stored.
 *
 * The IR therefore mirrors the frame exactly: pushing a value stores it at its
 * stack position, and popping loads it back. That is more instructions than
 * the bytecode had, and a later pass would keep short-lived values in
 * registers instead. Getting it right first is worth more than getting it
 * small: a wrong lowering is invisible until it produces a wrong number. */
static bool push(Lowering *low, IrBlock *block, int reg, int line) {
  if (low->stackTop >= IR_MAX_STACK) {
    low->reason = "expression stack too deep";
    return false;
  }
  /* The frame base — the callee and the arguments — is held as -1, because no
   * instruction in this function produced it. Duplicating one of those would
   * mean storing a register that does not exist. */
  if (reg < 0) {
    low->reason = "a frame slot was used as a value";
    return false;
  }
  IrInst *store = append(block, IR_STORE_LOCAL, line);
  store->a = low->stackTop;
  store->b = reg;
  low->slotType[low->stackTop] = low->ir->registerTypes[reg];
  if (low->stackTop + 1 > low->ir->slotCount) low->ir->slotCount = low->stackTop + 1;
  low->stack[low->stackTop++] = reg;
  return true;
}

static int pop(Lowering *low, IrBlock *block, int line) {
  if (low->stackTop == 0) {
    low->reason = "operand stack underflow while lowering";
    return 0;
  }
  low->stackTop--;
  int result = newRegister(low->ir, low->slotType[low->stackTop]);
  IrInst *load = append(block, IR_LOAD_LOCAL, line);
  load->type = low->slotType[low->stackTop];
  load->result = result;
  load->a = low->stackTop;
  /* A block entered at a height recorded from a jump can read a position no
   * push in this function ever wrote, so the slot count grows here too. */
  if (low->stackTop + 1 > low->ir->slotCount) low->ir->slotCount = low->stackTop + 1;
  return result;
}

/* The block a bytecode offset belongs to, by linear search over the starts —
 * fine for the sizes involved, and it keeps the mapping in one place. */
static int blockAt(const IrFunction *ir, int offset) {
  for (int i = 0; i < ir->blockCount; i++) {
    if (ir->blocks[i].bytecodeStart == offset) return i;
  }
  return -1;
}

static bool lowerBinary(Lowering *low, IrBlock *block, IrOp op, IrType type, int line) {
  int right = pop(low, block, line);
  int left = pop(low, block, line);
  if (low->reason != NULL) return false;

  int result = newRegister(low->ir, type);
  IrInst *inst = append(block, op, line);
  inst->result = result;
  inst->a = left;
  inst->b = right;
  inst->type = type;
  return push(low, block, result, line);
}

IrFunction *csIrLower(ObjFunction *function, const char **reason) {
  *reason = NULL;
  const Chunk *chunk = &function->chunk;

  if (chunk->count >= IR_MAX_BLOCKS * 8) {
    *reason = "function is too large";
    return NULL;
  }

  bool *leader = (bool *)calloc((size_t)chunk->count + 1, sizeof(bool));
  if (!markLeaders(chunk, leader, reason)) {
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
   * knows holds one, and every read of it is typed. */
  for (int i = 0; i < function->arity && i + 1 < IR_MAX_STACK; i++) {
    if (function->paramTypes != NULL && function->paramTypes[i] == TYPE_NUMBER) {
      low.slotType[i + 1] = IR_TYPE_NUMBER;
    }
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
      blockIndex = blockAt(ir, offset);
      /* A recorded height beats the linear one: it came from an actual jump. */
      if (blockIndex >= 0 && blockIndex < IR_MAX_BLOCKS &&
          low.entryHeight[blockIndex] >= 0) {
        low.stackTop = low.entryHeight[blockIndex];
      } else if (skipped) {
        /* Nothing reaches this block from the part being compiled, and its
         * height is unknown, so the whole of it is left to the interpreter —
         * up to the next block, which may have a height a jump recorded. */
        int skip = csInstructionLength(chunk, offset);
        if (skip <= offset) { low.reason = "could not be decoded"; goto failed; }
        while (skip < chunk->count && !leader[skip]) {
          int step = csInstructionLength(chunk, skip);
          if (step <= skip) { low.reason = "could not be decoded"; goto failed; }
          skip = step;
        }
        offset = skip;
        continue;
      }
      blockFloor = low.stackTop;
      floorOffset = offset;
      floorCount = 0;
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
    int wantsNumbers = 0;
    switch (opcode) {
      case OP_NEGATE: wantsNumbers = 1; break;
      case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY: case OP_DIVIDE:
      case OP_MODULO: case OP_LESS: case OP_LESS_EQUAL: case OP_GREATER:
      case OP_GREATER_EQUAL:
        wantsNumbers = 2;
        break;
      default: break;
    }
    for (int k = 0; k < wantsNumbers; k++) {
      if (low.stackTop - 1 - k < 0) {
        low.reason = "operand stack underflow while lowering";
        goto failed;
      }
      if (ir->registerTypes[low.stack[low.stackTop - 1 - k]] != IR_TYPE_NUMBER) {
        goto handOver;
      }
    }

    switch (opcode) {
      case OP_CONSTANT: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        Value constant = chunk->constants.values[index];
        IrType type = IS_NUMBER(constant)  ? IR_TYPE_NUMBER
                      : IS_BOOL(constant)  ? IR_TYPE_BOOL
                                           : IR_TYPE_UNKNOWN;
        int result = newRegister(ir, type);
        IrInst *inst = append(block, IR_CONST, line);
        inst->result = result;
        inst->constant = constant;
        inst->type = type;
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      /* Globals. Only where the module already holds a number under that name:
       * the checker will not let a declared binding change type, and nothing a
       * compiled region can do calls anything, so the only writer is this code
       * and every store it makes is proved numeric. Anything else hands the
       * frame back. */
      case OP_GET_GLOBAL: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        if (!globalHoldsNumber(function, chunk, index)) goto handOver;

        int result = newRegister(ir, IR_TYPE_NUMBER);
        IrInst *inst = append(block, IR_LOAD_GLOBAL, line);
        inst->result = result;
        inst->a = index;
        inst->type = IR_TYPE_NUMBER;
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      /* A definition is a store by the time this runs. The function only
       * lowers once it is hot, which is after the top level has already
       * defined its bindings — so the entry exists, and re-running the define
       * as a store to it is what the interpreter would do anyway. */
      case OP_DEFINE_GLOBAL:
      case OP_DEFINE_CONST:
      case OP_SET_GLOBAL:
      case OP_SET_GLOBAL_POP: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        if (!globalHoldsNumber(function, chunk, index)) goto handOver;
        if (low.stackTop < 1) {
          low.reason = "operand stack underflow while lowering";
          goto failed;
        }
        if (ir->registerTypes[low.stack[low.stackTop - 1]] != IR_TYPE_NUMBER) {
          goto handOver;
        }

        /* OP_SET_GLOBAL leaves the value; the other three consume it. */
        int value = opcode == OP_SET_GLOBAL ? low.stack[low.stackTop - 1]
                                            : pop(&low, block, line);
        if (low.reason != NULL) goto failed;

        IrInst *inst = append(block, IR_STORE_GLOBAL, line);
        inst->a = index;
        inst->b = value;
        break;
      }

      case OP_GET_LOCAL: {
        int slot = chunk->code[offset + 1];
        int result = newRegister(ir, low.slotType[slot]);
        IrInst *inst = append(block, IR_LOAD_LOCAL, line);
        inst->result = result;
        inst->a = slot;
        inst->type = low.slotType[slot];
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      case OP_GET_LOCAL_CONST: {
        int slot = chunk->code[offset + 1];
        int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
        int loaded = newRegister(ir, low.slotType[slot]);
        IrInst *load = append(block, IR_LOAD_LOCAL, line);
        load->result = loaded;
        load->a = slot;
        load->type = low.slotType[slot];
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        if (!push(&low, block, loaded, line)) goto failed;

        Value constant = chunk->constants.values[index];
        int result = newRegister(ir, IS_NUMBER(constant) ? IR_TYPE_NUMBER : IR_TYPE_UNKNOWN);
        IrInst *inst = append(block, IR_CONST, line);
        inst->result = result;
        inst->constant = constant;
        inst->type = ir->registerTypes[result];
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      /* The fused pair from the superinstruction work: two loads, and the
       * lowering has to know about every one of them or it refuses on ordinary
       * code that happens to have been optimised. */
      case OP_GET_LOCAL_LOCAL: {
        for (int which = 1; which <= 2; which++) {
          int slot = chunk->code[offset + which];
          int result = newRegister(ir, low.slotType[slot]);
          IrInst *inst = append(block, IR_LOAD_LOCAL, line);
          inst->result = result;
          inst->a = slot;
          inst->type = low.slotType[slot];
          if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
          if (!push(&low, block, result, line)) goto failed;
        }
        break;
      }

      case OP_SET_LOCAL: {
        /* Stores and leaves the value: assignment is an expression. */
        int slot = chunk->code[offset + 1];
        int value = low.stackTop > 0 ? low.stack[low.stackTop - 1] : -1;
        if (value < 0) { low.reason = "operand stack underflow while lowering"; goto failed; }
        IrInst *inst = append(block, IR_STORE_LOCAL, line);
        inst->a = slot;
        inst->b = value;
        low.slotType[slot] = low.slotType[slot] == IR_TYPE_UNKNOWN && ir->registerTypes[value] != IR_TYPE_UNKNOWN
                                 ? ir->registerTypes[value]
                                 : (low.slotType[slot] == ir->registerTypes[value] ? low.slotType[slot] : IR_TYPE_UNKNOWN);
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        break;
      }

      case OP_TRUE:
      case OP_FALSE:
      case OP_NULL:
      case OP_UNDEFINED: {
        IrType type = (opcode == OP_TRUE || opcode == OP_FALSE) ? IR_TYPE_BOOL
                                                                : IR_TYPE_UNKNOWN;
        int result = newRegister(ir, type);
        IrInst *inst = append(block, IR_CONST, line);
        inst->result = result;
        inst->constant = opcode == OP_TRUE    ? BOOL_VAL(true)
                         : opcode == OP_FALSE ? BOOL_VAL(false)
                         : opcode == OP_NULL  ? NULL_VAL
                                              : UNDEFINED_VAL;
        inst->type = type;
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      case OP_POP_N:
        for (int i = 0; i < chunk->code[offset + 1]; i++) {
          pop(&low, block, line);
          if (low.reason != NULL) goto failed;
        }
        break;

      case OP_DUP: {
        if (low.stackTop == 0) { low.reason = "operand stack underflow while lowering"; goto failed; }
        if (!push(&low, block, low.stack[low.stackTop - 1], line)) goto failed;
        break;
      }

      case OP_SET_LOCAL_POP: {
        int slot = chunk->code[offset + 1];
        int value = pop(&low, block, line);
        if (low.reason != NULL) goto failed;
        IrInst *inst = append(block, IR_STORE_LOCAL, line);
        inst->a = slot;
        inst->b = value;
        low.slotType[slot] = low.slotType[slot] == IR_TYPE_UNKNOWN && ir->registerTypes[value] != IR_TYPE_UNKNOWN
                                 ? ir->registerTypes[value]
                                 : (low.slotType[slot] == ir->registerTypes[value] ? low.slotType[slot] : IR_TYPE_UNKNOWN);
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        break;
      }

      case OP_INC_LOCAL:
      case OP_DEC_LOCAL: {
        int slot = chunk->code[offset + 1];
        int loaded = newRegister(ir, IR_TYPE_NUMBER);
        IrInst *load = append(block, IR_LOAD_LOCAL, line);
        load->result = loaded;
        load->a = slot;

        int one = newRegister(ir, IR_TYPE_NUMBER);
        IrInst *constant = append(block, IR_CONST, line);
        constant->result = one;
        constant->constant = NUMBER_VAL(1);
        constant->type = IR_TYPE_NUMBER;

        int sum = newRegister(ir, IR_TYPE_NUMBER);
        IrInst *add = append(block, opcode == OP_INC_LOCAL ? IR_ADD : IR_SUB, line);
        add->result = sum;
        add->a = loaded;
        add->b = one;
        add->type = IR_TYPE_NUMBER;

        IrInst *store = append(block, IR_STORE_LOCAL, line);
        store->a = slot;
        store->b = sum;
        if (slot + 1 > ir->slotCount) ir->slotCount = slot + 1;
        break;
      }

      case OP_ADD:
      case OP_ADD_NUM:
        /* Both are addition here. OP_ADD_NUM carries the checker's proof, and
         * plain OP_ADD only reaches this point at all when the guard above has
         * seen both operands typed as numbers — which is a proof of the same
         * thing, arrived at from the IR rather than from an annotation. A `+`
         * that might concatenate handed the frame back instead. */
        if (!lowerBinary(&low, block, IR_ADD, IR_TYPE_NUMBER, line)) goto failed;
        break;

      /* The rest of the arithmetic requires numbers by definition — the VM
       * errors otherwise — so the result type is known without an annotation. */
      case OP_SUBTRACT:
        if (!lowerBinary(&low, block, IR_SUB, IR_TYPE_NUMBER, line)) goto failed;
        break;
      case OP_MULTIPLY:
        if (!lowerBinary(&low, block, IR_MUL, IR_TYPE_NUMBER, line)) goto failed;
        break;
      case OP_DIVIDE:
        if (!lowerBinary(&low, block, IR_DIV, IR_TYPE_NUMBER, line)) goto failed;
        break;
      case OP_MODULO:
        if (!lowerBinary(&low, block, IR_MOD, IR_TYPE_NUMBER, line)) goto failed;
        break;

      case OP_LESS:
        if (!lowerBinary(&low, block, IR_LT, IR_TYPE_BOOL, line)) goto failed;
        break;
      case OP_LESS_EQUAL:
        if (!lowerBinary(&low, block, IR_LE, IR_TYPE_BOOL, line)) goto failed;
        break;
      case OP_GREATER:
        if (!lowerBinary(&low, block, IR_GT, IR_TYPE_BOOL, line)) goto failed;
        break;
      case OP_GREATER_EQUAL:
        if (!lowerBinary(&low, block, IR_GE, IR_TYPE_BOOL, line)) goto failed;
        break;
      case OP_EQUAL:
        if (!lowerBinary(&low, block, IR_EQ, IR_TYPE_BOOL, line)) goto failed;
        break;
      case OP_NOT_EQUAL:
        if (!lowerBinary(&low, block, IR_NE, IR_TYPE_BOOL, line)) goto failed;
        break;

      case OP_NEGATE: {
        int operand = pop(&low, block, line);
        if (low.reason != NULL) goto failed;
        int result = newRegister(ir, IR_TYPE_NUMBER);
        IrInst *inst = append(block, IR_NEG, line);
        inst->result = result;
        inst->a = operand;
        inst->type = IR_TYPE_NUMBER;
        if (!push(&low, block, result, line)) goto failed;
        break;
      }

      /* A fused compare-and-branch is two IR instructions: the comparison and
       * the branch on it. Splitting them is what lets a backend keep the
       * comparison in flags. */
      case OP_JUMP_IF_NOT_LESS:
      case OP_JUMP_IF_NOT_LESS_EQUAL:
      case OP_JUMP_IF_NOT_GREATER:
      case OP_JUMP_IF_NOT_GREATER_EQUAL:
      case OP_JUMP_IF_NOT_EQUAL:
      case OP_JUMP_IF_EQUAL: {
        IrOp comparison = opcode == OP_JUMP_IF_NOT_LESS          ? IR_LT
                          : opcode == OP_JUMP_IF_NOT_LESS_EQUAL  ? IR_LE
                          : opcode == OP_JUMP_IF_NOT_GREATER     ? IR_GT
                          : opcode == OP_JUMP_IF_NOT_GREATER_EQUAL ? IR_GE
                          : opcode == OP_JUMP_IF_EQUAL           ? IR_NE
                                                                 : IR_EQ;
        if (!lowerBinary(&low, block, comparison, IR_TYPE_BOOL, line)) goto failed;
        int condition = pop(&low, block, line);

        IrInst *branch = append(block, IR_BRANCH, line);
        branch->a = condition;
        /* These jump when the comparison is *false*, which is why the arms
         * read the way they do: taken means fall through. */
        branch->b = blockAt(ir, next);
        branch->c = blockAt(ir, jumpTarget);
        if (!reachBlock(&low, branch->b, low.stackTop)) goto failed;
        if (!reachBlock(&low, branch->c, low.stackTop)) goto failed;
        break;
      }

      case OP_LOOP: {
        int target = next - ((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        IrInst *jump = append(block, IR_JUMP, line);
        jump->a = blockAt(ir, target);
        if (!reachBlock(&low, jump->a, low.stackTop)) goto failed;
        break;
      }

      case OP_JUMP: {
        IrInst *jump = append(block, IR_JUMP, line);
        jump->a = blockAt(ir, jumpTarget);
        if (!reachBlock(&low, jump->a, low.stackTop)) goto failed;
        break;
      }

      case OP_POP_JUMP_IF_FALSE:
      case OP_JUMP_IF_FALSE:
      case OP_JUMP_IF_TRUE: {
        int condition = opcode == OP_POP_JUMP_IF_FALSE
                            ? pop(&low, block, line)
                            : low.stack[low.stackTop - 1]; /* these leave it */
        if (low.reason != NULL) goto failed;

        IrInst *branch = append(block, IR_BRANCH, line);
        branch->a = condition;
        if (opcode == OP_JUMP_IF_TRUE) {
          branch->b = blockAt(ir, jumpTarget);
          branch->c = blockAt(ir, next);
        } else {
          branch->b = blockAt(ir, next);
          branch->c = blockAt(ir, jumpTarget);
        }
        if (!reachBlock(&low, branch->b, low.stackTop)) goto failed;
        if (!reachBlock(&low, branch->c, low.stackTop)) goto failed;
        break;
      }

      case OP_RETURN: {
        int value = pop(&low, block, line);
        if (low.reason != NULL) goto failed;
        IrInst *inst = append(block, IR_RETURN, line);
        inst->a = value;
        break;
      }

      case OP_POP:
        pop(&low, block, line);
        if (low.reason != NULL) goto failed;
        break;

      default:
        goto handOver;
    }

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
      if (blockFloor < 0 || blockFloor >= IR_MAX_STACK ||
          floorCount > block->count) {
        low.reason = csOpcodeName((OpCode)opcode);
        goto failed;
      }
      block->count = floorCount;

      IrInst *exit = append(block, IR_EXIT, line);
      exit->a = floorOffset;
      exit->b = blockFloor;
      ir->hasExits = true;

      /* Everything up to the next jump target is the interpreter's now.
       * Blocks past it are still lowered: a loop whose body this compiler
       * understands is usually followed by code it does not. */
      skipped = true;
      int skip = next;
      while (skip < chunk->count && !leader[skip]) {
        int step = csInstructionLength(chunk, skip);
        if (step <= skip) { low.reason = "could not be decoded"; goto failed; }
        skip = step;
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

    IrInst *exit = append(&ir->blocks[b], IR_EXIT, 0);
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
      for (int i = 0; i < ir->blocks[b].count; i++) {
        const IrInst *inst = &ir->blocks[b].instructions[i];
        int targets[2] = {-1, -1};
        if (inst->op == IR_JUMP) targets[0] = inst->a;
        if (inst->op == IR_BRANCH) { targets[0] = inst->b; targets[1] = inst->c; }
        for (int k = 0; k < 2; k++) {
          int to = targets[k];
          if (to < 0 || to >= ir->blockCount || reachable[to]) continue;
          reachable[to] = true;
          grew = true;
        }
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

void csIrRegisterOperands(const IrInst *inst, int *a, int *b) {
  *a = -1;
  *b = -1;

  switch (inst->op) {
    case IR_CONST:
    case IR_JUMP:
    case IR_EXIT:
    case IR_LOAD_LOCAL:
    case IR_LOAD_GLOBAL:
      break; /* neither field is a register */

    case IR_STORE_LOCAL:
    case IR_STORE_GLOBAL:
      *b = inst->b; /* `a` is the destination, not a value */
      break;

    case IR_NEG:
    case IR_RETURN:
    case IR_BRANCH:
      *a = inst->a; /* a branch's `b` and `c` are blocks */
      break;

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
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

      if (inst.op == IR_LOAD_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK &&
          slotHolder[inst.a] >= 0) {
        /* The value is already in a register: rename and drop the load. */
        rename[inst.result] = slotHolder[inst.a];
        continue;
      }

      if (inst.op == IR_STORE_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK) {
        slotHolder[inst.a] = inst.b;
      } else if (inst.op == IR_LOAD_LOCAL && inst.a >= 0 && inst.a < IR_MAX_STACK) {
        slotHolder[inst.a] = inst.result;
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

      /* An exit reads everything. The interpreter picks the frame up from
       * there and its next instruction may be a load of any live slot — a
       * read this pass cannot see, because it is not in the IR at all.
       * Removing those stores made a loop compute the right answer and then
       * hand back the value it started with. */
      if (inst->op == IR_EXIT) {
        for (int s = 0; s <= ir->slotCount && s < inst->b; s++) isRead[s] = true;
      }
    }
  }

  for (int b = 0; b < ir->blockCount; b++) {
    IrBlock *block = &ir->blocks[b];
    int kept = 0;
    for (int i = 0; i < block->count; i++) {
      const IrInst *inst = &block->instructions[i];
      if (inst->op == IR_STORE_LOCAL && inst->a >= 0 && inst->a <= ir->slotCount &&
          !isRead[inst->a]) {
        continue;
      }
      block->instructions[kept++] = block->instructions[i];
    }
    block->count = kept;
  }

  free(isRead);
}

/* Makes each slot's type the meet of everything stored into it, and downgrades
 * every load that claimed more.
 *
 * The lowering tracks slot types in linear order, which is not sound on its
 * own: a slot read where it happens to hold a number is typed `number` even
 * when another store puts something else there — and a loop back-edge makes
 * "another store" mean "the previous iteration". The result was a load typed
 * `number` reading a boolean, and arithmetic compiled with no guard to check.
 *
 * Iterated because downgrading a load downgrades what is computed from it,
 * which can downgrade the slot the result is stored to in turn. It settles
 * quickly: every round can only remove information. */
void csIrReconcileSlotTypes(IrFunction *ir) {
  int slots = ir->slotCount + 1;
  IrType *meet = (IrType *)malloc(sizeof(IrType) * (size_t)slots);
  bool *written = (bool *)malloc(sizeof(bool) * (size_t)slots);

  for (int round = 0; round < 8; round++) {
    for (int s = 0; s < slots; s++) {
      meet[s] = IR_TYPE_UNKNOWN;
      written[s] = false;
    }

    for (int b = 0; b < ir->blockCount; b++) {
      for (int i = 0; i < ir->blocks[b].count; i++) {
        const IrInst *inst = &ir->blocks[b].instructions[i];
        if (inst->op != IR_STORE_LOCAL) continue;
        if (inst->a < 0 || inst->a >= slots) continue;
        if (inst->b < 0 || inst->b > ir->registerCount) continue;

        IrType stored = ir->registerTypes[inst->b];
        if (!written[inst->a]) {
          meet[inst->a] = stored;
          written[inst->a] = true;
        } else if (meet[inst->a] != stored) {
          meet[inst->a] = IR_TYPE_UNKNOWN;
        }
      }
    }

    /* A slot nothing stores to holds whatever the caller or the interpreter
     * left there, which is only known for an annotated parameter. */
    for (int s = 0; s < slots; s++) {
      if (!written[s]) meet[s] = ir->slotTypes[s];
    }

    bool changed = false;
    for (int b = 0; b < ir->blockCount; b++) {
      for (int i = 0; i < ir->blocks[b].count; i++) {
        IrInst *inst = &ir->blocks[b].instructions[i];
        if (inst->op != IR_LOAD_LOCAL) continue;
        if (inst->a < 0 || inst->a >= slots) continue;
        if (inst->type == meet[inst->a]) continue;

        inst->type = meet[inst->a];
        if (inst->result >= 0 && inst->result <= ir->registerCount) {
          ir->registerTypes[inst->result] = meet[inst->a];
        }
        changed = true;
      }
    }

    for (int s = 0; s < slots; s++) ir->slotTypes[s] = meet[s];
    if (!changed) break;
  }

  free(meet);
  free(written);
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
  bool sawArithmetic = false;
  for (int b = 0; b < ir->blockCount; b++) {
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      switch (inst->op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
          if (ir->registerTypes[inst->a] != IR_TYPE_NUMBER) return false;
          if (ir->registerTypes[inst->b] != IR_TYPE_NUMBER) return false;
          sawArithmetic = true;
          break;
        case IR_NEG:
          if (ir->registerTypes[inst->a] != IR_TYPE_NUMBER) return false;
          sawArithmetic = true;
          break;
        case IR_BRANCH:
          /* A branch has to be on something the IR can test. */
          if (ir->registerTypes[inst->a] != IR_TYPE_BOOL) return false;
          break;
        default: break;
      }
    }
  }
  return sawArithmetic;
}

void csIrFree(IrFunction *ir) {
  if (ir == NULL) return;
  for (int i = 0; i < ir->blockCount; i++) free(ir->blocks[i].instructions);
  free(ir->blocks);
  free(ir->registerTypes);
  free(ir->slotTypes);
  free(ir);
}

/* ---- printing ---------------------------------------------------------- */

static const char *opName(IrOp op) {
  switch (op) {
    case IR_CONST: return "const";
    case IR_LOAD_LOCAL: return "load";
    case IR_STORE_LOCAL: return "store";
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_DIV: return "div";
    case IR_MOD: return "mod";
    case IR_NEG: return "neg";
    case IR_LT: return "lt";
    case IR_LE: return "le";
    case IR_GT: return "gt";
    case IR_GE: return "ge";
    case IR_EQ: return "eq";
    case IR_NE: return "ne";
    case IR_JUMP: return "jump";
    case IR_BRANCH: return "branch";
    case IR_RETURN: return "return";
    case IR_EXIT:   return "exit";
    case IR_LOAD_GLOBAL:  return "loadg";
    case IR_STORE_GLOBAL: return "storeg";
  }
  return "?";
}

static const char *typeName(IrType type) {
  switch (type) {
    case IR_TYPE_NUMBER: return "num";
    case IR_TYPE_BOOL: return "bool";
    default: return "val";
  }
}

void csIrPrint(const IrFunction *ir) {
  printf("  ir for %s: %d block%s, %d registers, %d slots\n",
         ir->source->name != NULL ? ir->source->name->chars : "<top level>",
         ir->blockCount, ir->blockCount == 1 ? "" : "s", ir->registerCount,
         ir->slotCount);

  for (int b = 0; b < ir->blockCount; b++) {
    printf("    block %d:\n", b);
    for (int i = 0; i < ir->blocks[b].count; i++) {
      const IrInst *inst = &ir->blocks[b].instructions[i];
      printf("      ");
      if (inst->result >= 0) {
        printf("r%-3d:%-4s = ", inst->result, typeName(inst->type));
      } else {
        printf("%14s", "");
      }
      printf("%-7s", opName(inst->op));

      switch (inst->op) {
        case IR_CONST:
          printf(" ");
          csValuePrint(inst->constant);
          break;
        case IR_LOAD_LOCAL: printf(" slot%d", inst->a); break;
        case IR_STORE_LOCAL: printf(" slot%d, r%d", inst->a, inst->b); break;
        case IR_JUMP: printf(" block%d", inst->a); break;
        case IR_BRANCH: printf(" r%d ? block%d : block%d", inst->a, inst->b, inst->c); break;
        case IR_RETURN: printf(" r%d", inst->a); break;
        case IR_EXIT: printf("  -> bytecode %d, stack %d", inst->a, inst->b); break;
        case IR_LOAD_GLOBAL: printf(" global%d", inst->a); break;
        case IR_STORE_GLOBAL: printf(" global%d, r%d", inst->a, inst->b); break;
        case IR_NEG: printf(" r%d", inst->a); break;
        default: printf(" r%d, r%d", inst->a, inst->b); break;
      }
      printf("\n");
    }
  }
}

/* ---- running it -------------------------------------------------------- */

/* An interpreter for the IR.
 *
 * This exists to be thrown away. Its purpose is to answer one question before
 * a code generator is written: does the lowering mean the same thing as the
 * bytecode it came from? Running both and comparing is a far cheaper way to
 * find a mistranslation than finding it in machine code, where the symptom is
 * a wrong number and the cause is three layers down.
 */
#define IR_MAX_SLOTS 256
#define IR_MAX_STEPS 200000000

bool csIrInterpret(const IrFunction *ir, const Value *args, int argCount, Value *out) {
  if (ir->slotCount > IR_MAX_SLOTS) return false;

  Value slots[IR_MAX_SLOTS];
  for (int i = 0; i < ir->slotCount; i++) slots[i] = UNDEFINED_VAL;
  /* Slot 0 is the callee, as in a real frame; the arguments follow it. */
  for (int i = 0; i < argCount && i + 1 < ir->slotCount; i++) slots[i + 1] = args[i];

  Value *registers = (Value *)malloc(sizeof(Value) * (size_t)(ir->registerCount + 1));
  for (int i = 0; i < ir->registerCount; i++) registers[i] = UNDEFINED_VAL;

  int block = 0;
  long steps = 0;
  bool ok = false;

  while (block >= 0 && block < ir->blockCount) {
    const IrBlock *current = &ir->blocks[block];
    int nextBlock = block + 1; /* falling off the end runs into the next */

    for (int i = 0; i < current->count; i++) {
      if (++steps > IR_MAX_STEPS) goto done;
      const IrInst *inst = &current->instructions[i];

      /* Bounds are checked rather than assumed. A lowering bug should surface
       * as a refusal — and a failing test — rather than as a crash, which says
       * far less about where it came from. */
      if (inst->result >= ir->registerCount) goto done;
      if ((inst->op == IR_LOAD_LOCAL || inst->op == IR_STORE_LOCAL) &&
          (inst->a < 0 || inst->a >= ir->slotCount)) {
        goto done;
      }
      /* Every register operand, not only the result. A negative one means the
       * lowering referred to a value it never produced. */
      if (inst->op == IR_STORE_LOCAL && (inst->b < 0 || inst->b >= ir->registerCount)) {
        goto done;
      }
      if (inst->op == IR_EXIT) goto done;
      if (inst->op == IR_STORE_GLOBAL &&
          (inst->b < 0 || inst->b >= ir->registerCount)) {
        goto done;
      }
      if (inst->op != IR_CONST && inst->op != IR_LOAD_LOCAL &&
          inst->op != IR_STORE_LOCAL && inst->op != IR_JUMP &&
          inst->op != IR_LOAD_GLOBAL && inst->op != IR_STORE_GLOBAL) {
        if (inst->a < 0 || inst->a >= ir->registerCount) goto done;
        if (inst->op != IR_NEG && inst->op != IR_RETURN && inst->op != IR_BRANCH &&
            (inst->b < 0 || inst->b >= ir->registerCount)) {
          goto done;
        }
      }

      switch (inst->op) {
        case IR_CONST: registers[inst->result] = inst->constant; break;
        case IR_LOAD_LOCAL: registers[inst->result] = slots[inst->a]; break;
        case IR_STORE_LOCAL: slots[inst->a] = registers[inst->b]; break;

        case IR_ADD:
          /* The one operator that is not arithmetic when a string is involved.
           * The lowering records which case it is in the result type. */
          if (inst->type != IR_TYPE_NUMBER &&
              (!IS_NUMBER(registers[inst->a]) || !IS_NUMBER(registers[inst->b]))) {
            goto done; /* leave it to the bytecode VM */
          }
          registers[inst->result] =
              NUMBER_VAL(AS_NUMBER(registers[inst->a]) + AS_NUMBER(registers[inst->b]));
          break;

        case IR_SUB:
          registers[inst->result] =
              NUMBER_VAL(AS_NUMBER(registers[inst->a]) - AS_NUMBER(registers[inst->b]));
          break;
        case IR_MUL:
          registers[inst->result] =
              NUMBER_VAL(AS_NUMBER(registers[inst->a]) * AS_NUMBER(registers[inst->b]));
          break;
        case IR_DIV:
          registers[inst->result] =
              NUMBER_VAL(AS_NUMBER(registers[inst->a]) / AS_NUMBER(registers[inst->b]));
          break;
        case IR_MOD: {
          double x = AS_NUMBER(registers[inst->a]);
          double y = AS_NUMBER(registers[inst->b]);
          /* The same integer fast path the VM takes, for the same reason. */
          if (x >= 0 && y > 0 && x == (double)(long long)x && y == (double)(long long)y) {
            registers[inst->result] = NUMBER_VAL((double)((long long)x % (long long)y));
          } else {
            registers[inst->result] = NUMBER_VAL(fmod(x, y));
          }
          break;
        }
        case IR_NEG:
          registers[inst->result] = NUMBER_VAL(-AS_NUMBER(registers[inst->a]));
          break;

        case IR_LT:
          registers[inst->result] =
              BOOL_VAL(AS_NUMBER(registers[inst->a]) < AS_NUMBER(registers[inst->b]));
          break;
        case IR_LE:
          registers[inst->result] =
              BOOL_VAL(AS_NUMBER(registers[inst->a]) <= AS_NUMBER(registers[inst->b]));
          break;
        case IR_GT:
          registers[inst->result] =
              BOOL_VAL(AS_NUMBER(registers[inst->a]) > AS_NUMBER(registers[inst->b]));
          break;
        case IR_GE:
          registers[inst->result] =
              BOOL_VAL(AS_NUMBER(registers[inst->a]) >= AS_NUMBER(registers[inst->b]));
          break;
        case IR_EQ:
          registers[inst->result] =
              BOOL_VAL(csValuesStrictEqual(registers[inst->a], registers[inst->b]));
          break;
        case IR_NE:
          registers[inst->result] =
              BOOL_VAL(!csValuesStrictEqual(registers[inst->a], registers[inst->b]));
          break;

        case IR_JUMP:
          nextBlock = inst->a;
          goto blockDone;
        case IR_BRANCH:
          nextBlock = AS_BOOL(registers[inst->a]) ? inst->b : inst->c;
          goto blockDone;
        case IR_LOAD_GLOBAL: {
          Value key = ir->source->chunk.constants.values[inst->a];
          Value held;
          if (!IS_STRING(key) || ir->source->module == NULL ||
              !csTableGet(&ir->source->module->globals, AS_STRING(key), &held)) {
            goto done;
          }
          registers[inst->result] = held;
          break;
        }

        case IR_STORE_GLOBAL: {
          Value key = ir->source->chunk.constants.values[inst->a];
          if (!IS_STRING(key) || ir->source->module == NULL) goto done;
          csTableSet(&ir->source->module->globals, AS_STRING(key),
                     registers[inst->b]);
          break;
        }

        case IR_RETURN:
          *out = registers[inst->a];
          ok = true;
          goto done;

        case IR_EXIT:
          /* This interpreter runs a whole function in place of the bytecode,
           * so it has nowhere to hand a half-finished frame back to. Only
           * machine code entered through OSR can take an exit; here it simply
           * means the bytecode should have run instead. */
          goto done;
      }
    }

  blockDone:
    block = nextBlock;
  }

done:
  free(registers);
  return ok;
}
