/* ir_inline.c — splicing a small callee's body in where the call was.
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

/* ---- inlining -----------------------------------------------------------
 *
 * A call is the thing this compiler cannot do. Compiled code has no frame of
 * its own and no safepoint at which the collector could walk one, so calling
 * out means building both — and until they exist, one call in a loop body is
 * enough to hand the whole loop back to the interpreter.
 *
 * Splicing the callee's body in where the call was needs neither. The bodies
 * this admits are straight-line arithmetic over their own parameters: no
 * branches, no calls of their own, nothing that can throw past the caller. So
 * the callee's frame never has to exist at all — its slots become a
 * compile-time map onto registers the caller already holds, and what is left
 * is arithmetic the register allocator sees as if it had been written inline.
 *
 * That is the whole of the gain in `bench/jit/jit_calls`: the driving loop is
 * not slow because the call is slow, it is slow because the call stopped the
 * loop from compiling.
 */

/* How much body is worth splicing, per call and per caller. Both bounds are
 * arbitrary and both are the point: inlining trades code size for calls
 * removed, and without a limit a chain of small functions is one large one. */

/* The instructions a spliceable body may be made of.
 *
 * Deliberately narrow. Every one of these reads and writes only the callee's
 * own frame positions, cannot branch, cannot allocate and cannot throw past
 * the caller — which is what makes the frame the interpreter would have built
 * unobservable, and therefore optional. */
static bool inlinableOpcode(uint8_t opcode) {
  switch (opcode) {
    case OP_CONSTANT:
    case OP_NULL:
    case OP_UNDEFINED:
    case OP_TRUE:
    case OP_FALSE:
    case OP_GET_LOCAL:
    case OP_GET_LOCAL_LOCAL:
    case OP_GET_LOCAL_CONST:
    case OP_SET_LOCAL:
    case OP_SET_LOCAL_POP:
    case OP_INC_LOCAL:
    case OP_DEC_LOCAL:
    case OP_DUP:
    case OP_POP:
    case OP_ADD:
    case OP_ADD_NUM:
    case OP_SUBTRACT:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MODULO:
    case OP_NEGATE:
    case OP_LESS:
    case OP_LESS_EQUAL:
    case OP_GREATER:
    case OP_GREATER_EQUAL:
    case OP_EQUAL:
    case OP_NOT_EQUAL: return true;
    default: return false;
  }
}

/* Whether this function's body can go where a call to it is.
 *
 * Structural only, and answered before anything is emitted. The arity has to
 * match exactly: a call the VM would have refused for passing too few
 * arguments must still be refused, and a default parameter is applied by the
 * frame the VM builds — which is the frame this is removing. */
/* Whether a call to this closure can be *made* — as against spliced.
 *
 * The same structural conditions the splice needs about the frame the VM would
 * have built, and none of the ones about the body: what makes a call worth
 * emitting is precisely that the body is more than the splice will take.
 *
 * A generator or an async function answers something other than what its body
 * returns, and a method needs a receiver this has no value for. All three are
 * left to the interpreter. */
bool csIrCalleeIsCallable(const ObjFunction *callee, int argCount) {
  if (callee == NULL) return false;
  if (callee->arity != argCount || callee->paramCount != argCount) return false;
  if (callee->hasRest) return false;
  if (callee->isAsync || callee->isGenerator || callee->isMethod) return false;
  if (callee->usesThis) return false;
  return argCount + 1 < IR_MAX_STACK;
}

bool csIrCalleeIsInlinable(const ObjFunction *callee, int argCount) {
  if (callee == NULL) return false;
  if (callee->arity != argCount || callee->paramCount != argCount) return false;
  if (callee->hasRest || callee->upvalueCount != 0) return false;
  if (callee->isAsync || callee->isGenerator || callee->isMethod) return false;
  /* `this` is slot 0, which a spliced body has no value for: an ordinary call
   * blanks it, and the map below deliberately leaves it empty. */
  if (callee->usesThis) return false;
  if (argCount + 1 >= IR_MAX_STACK) return false;

  const Chunk *chunk = &callee->chunk;
  int count = 0;
  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    /* The first return ends the body. Everything after it is the `return
     * undefined` the compiler appends to every function, which is unreachable
     * precisely because this one is straight-line. */
    if (opcode == OP_RETURN) return count > 0;
    if (!inlinableOpcode(opcode)) return false;
    if (++count > IR_INLINE_MAX_OPS) return false;

    int next = csInstructionLength(chunk, offset);
    if (next <= offset) return false;
    offset = next;
  }
  return false; /* no return reached, so not straight-line after all */
}

/* What one instruction of an argument expression does to the operand stack.
 *
 * Only the forms an argument can be built from, because this exists to find
 * one call rather than to model the language. Anything else abandons the
 * match, and the callee load hands the frame back exactly as it did before. */
static bool argumentStackEffect(uint8_t opcode, int *effect) {
  switch (opcode) {
    case OP_CONSTANT:
    case OP_NULL:
    case OP_UNDEFINED:
    case OP_TRUE:
    case OP_FALSE:
    case OP_GET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_DUP:
    case OP_GET_LOCAL_PROPERTY: *effect = 1; return true;
    case OP_GET_LOCAL_CONST:
    case OP_GET_LOCAL_LOCAL: *effect = 2; return true;
    case OP_GET_PROPERTY:
    case OP_NEGATE: *effect = 0; return true;
    case OP_ADD:
    case OP_ADD_NUM:
    case OP_SUBTRACT:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MODULO:
    case OP_LESS:
    case OP_LESS_EQUAL:
    case OP_GREATER:
    case OP_GREATER_EQUAL:
    case OP_EQUAL:
    case OP_NOT_EQUAL:
    case OP_POP: *effect = -1; return true;
    default: return false;
  }
}

/* Where the call a callee load feeds is, or -1.
 *
 * This is what makes the placeholder safe rather than merely convenient. A
 * position on the abstract stack that holds no register has to be taken off by
 * the call it was pushed for, and nothing else in this lowering could do it —
 * so the call is found first, in the same straight run, with the right number
 * of arguments between the two. Where it is not found the callee load hands
 * the frame back, which is what it did before inlining existed. */
int csIrCallSiteFor(const Chunk *chunk, const bool *leader, int calleeOffset, int *argCountOut) {
  int depth = 0;
  int offset = csInstructionLength(chunk, calleeOffset);

  for (int steps = 0; steps < IR_INLINE_MAX_OPS * 2; steps++) {
    if (offset <= calleeOffset || offset >= chunk->count) return -1;
    /* A jump lands here, so the stack the walk is modelling is not the only
     * one that reaches this point. */
    if (leader[offset]) return -1;

    uint8_t opcode = chunk->code[offset];
    if (opcode == OP_CALL) {
      int argCount = chunk->code[offset + 1];
      if (depth == argCount) {
        *argCountOut = argCount;
        return offset;
      }
      /* A call nested inside an argument, with a callee of its own below its
       * arguments. It leaves one value where argCount + 1 were. */
      if (depth < argCount + 1) return -1;
      depth -= argCount;
    } else {
      int effect;
      if (!argumentStackEffect(opcode, &effect)) return -1;
      depth += effect;
      if (depth < 0) return -1;
    }

    int next = csInstructionLength(chunk, offset);
    if (next <= offset) return -1;
    offset = next;
  }
  return -1;
}

/* Records the binding an inlined callee was read from, once per callee. */
bool csIrRememberInlinedCall(IrFunction *ir, Table *globals, ObjString *name, ObjClosure *callee) {
  for (int i = 0; i < ir->inlinedCount; i++) {
    if (ir->inlined[i].name == name && ir->inlined[i].callee == callee) return true;
    /* One name, one callee. Two closures under the same binding would mean the
     * binding changed while the function was being lowered, which nothing in a
     * single-threaded run can do — but assuming it is a worse foundation than
     * refusing. */
    if (ir->inlined[i].name == name) return false;
  }

  if (ir->inlinedCount == ir->inlinedCapacity) {
    int capacity = ir->inlinedCapacity < 4 ? 4 : ir->inlinedCapacity * 2;
    IrInlinedCall *grown = (IrInlinedCall *)realloc(ir->inlined, sizeof(IrInlinedCall) * (size_t)capacity);
    if (grown == NULL) return false;
    ir->inlined = grown;
    ir->inlinedCapacity = capacity;
  }

  ir->inlined[ir->inlinedCount].globals = globals;
  ir->inlined[ir->inlinedCount].name = name;
  ir->inlined[ir->inlinedCount].callee = callee;
  ir->inlinedCount++;
  return true;
}

bool csIrInlinedCalleesHold(const IrFunction *ir) {
  for (int i = 0; i < ir->inlinedCount; i++) {
    const IrInlinedCall *call = &ir->inlined[i];
    Value current;
    if (!csTableGet(call->globals, call->name, &current)) return false;
    if (!IS_CLOSURE(current) || AS_CLOSURE(current) != call->callee) return false;
  }
  return true;
}

void csIrMarkReferences(const IrFunction *ir) {
  for (int i = 0; i < ir->entryShapeCount; i++) {
    csMarkObject((Obj *)ir->entryShapes[i].shape);
  }

  /* The callees this function calls without splicing, and the names they were
   * read from — the same pair an inlined call keeps, for the same reason. */
  for (int i = 0; i < ir->callCount; i++) {
    csMarkObject((Obj *)ir->calls[i].name);
    csMarkObject((Obj *)ir->calls[i].callee);
  }

  /* The keys of every object literal this function builds: compiled code holds
   * their addresses and has no chunk to find them in. */
  for (int i = 0; i < ir->literalCount; i++) {
    for (int k = 0; k < ir->literals[i].count; k++) {
      csMarkObject((Obj *)ir->literals[i].keys[k]);
    }
  }

  /* The layout an added property gives the object. A transition edge is weak,
   * so between compiling the store and running it nothing else would keep the
   * new shape alive — and the compiled code holds its address. */
  for (int b = 0; b < ir->blockCount; b++) {
    const IrBlock *block = &ir->blocks[b];
    for (int i = 0; i < block->count; i++) {
      if (block->instructions[i].op != IR_ADD_PROPERTY) continue;
      csMarkObject(AS_OBJ(block->instructions[i].constant));
    }
  }
  for (int i = 0; i < ir->inlinedCount; i++) {
    csMarkObject((Obj *)ir->inlined[i].name);
    csMarkObject((Obj *)ir->inlined[i].callee);
  }
}

/* Splices a callee's body into the block the call was being lowered into.
 *
 * The callee's frame becomes a compile-time map: position k + 1 is the
 * register holding argument k, and a local it declares is whatever register
 * the expression that declared it produced. Nothing is stored to a frame slot
 * and nothing is loaded back, because there is no frame — the whole of the
 * callee's state is the caller's registers, which is exactly what makes this
 * possible without the safepoints a real call would need.
 *
 * Returns the register holding the call's result, or -1 when the body turns
 * out to hold something this cannot express. Instructions may already have
 * been emitted in that case; the caller throws them away. */
int csIrInlineCallee(IrFunction *ir, IrBlock *block, const ObjFunction *callee, const int *args, int argCount, int line) {
  const Chunk *chunk = &callee->chunk;

  int map[IR_MAX_STACK];
  for (int s = 0; s < IR_MAX_STACK; s++) map[s] = -1;
  /* Slot 0 stays empty: it is the callee itself in a frame that is not being
   * built, and calleeIsInlinable has already refused any body that reads it. */
  for (int a = 0; a < argCount; a++) map[a + 1] = args[a];

  const int floor = argCount + 1;
  int top = floor;
  int emitted = 0;

  for (int offset = 0; offset < chunk->count;) {
    uint8_t opcode = chunk->code[offset];
    int next = csInstructionLength(chunk, offset);
    if (next <= offset) return -1;

    /* Every operand of an operation that needs numbers has to be proved one,
     * exactly as in the code around the splice. A spliced body is held to the
     * same standard as one that was written inline, which is the only way the
     * result can be the same. */
    int wantsNumbers = 0;
    switch (opcode) {
      case OP_NEGATE: wantsNumbers = 1; break;
      case OP_ADD:
      case OP_ADD_NUM:
      case OP_SUBTRACT:
      case OP_MULTIPLY:
      case OP_DIVIDE:
      case OP_MODULO:
      case OP_LESS:
      case OP_LESS_EQUAL:
      case OP_GREATER:
      case OP_GREATER_EQUAL:
      /* Equality is here and not in the outer lowering because the backend
       * compares two doubles: on anything else that is a floating-point
       * compare of NaN-boxed bits, which is not what `===` means. */
      case OP_EQUAL:
      case OP_NOT_EQUAL: wantsNumbers = 2; break;
      default: break;
    }
    for (int k = 0; k < wantsNumbers; k++) {
      if (top - 1 - k < floor) return -1;
      int reg = map[top - 1 - k];
      if (reg < 0 || ir->registerTypes[reg] != IR_TYPE_NUMBER) return -1;
    }

    switch (opcode) {
      case OP_RETURN: {
        if (top <= floor) return -1;
        ir->inlinedInstructions += emitted;
        return map[top - 1];
      }

      case OP_CONSTANT:
      case OP_NULL:
      case OP_UNDEFINED:
      case OP_TRUE:
      case OP_FALSE: {
        Value constant = UNDEFINED_VAL;
        if (opcode == OP_CONSTANT) {
          int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
          if (index < 0 || index >= chunk->constants.count) return -1;
          constant = chunk->constants.values[index];
        } else if (opcode == OP_NULL) {
          constant = NULL_VAL;
        } else if (opcode == OP_TRUE || opcode == OP_FALSE) {
          constant = BOOL_VAL(opcode == OP_TRUE);
        }

        IrType type = IS_NUMBER(constant) ? IR_TYPE_NUMBER : IS_BOOL(constant) ? IR_TYPE_BOOL : IR_TYPE_UNKNOWN;
        if (top >= IR_MAX_STACK) return -1;
        int result = csIrNewRegister(ir, type);
        IrInst *inst = csIrAppend(block, IR_CONST, line);
        inst->result = result;
        inst->constant = constant;
        inst->type = type;
        map[top++] = result;
        break;
      }

      case OP_GET_LOCAL:
      case OP_GET_LOCAL_LOCAL:
      case OP_GET_LOCAL_CONST: {
        int wanted = opcode == OP_GET_LOCAL_LOCAL ? 2 : 1;
        for (int which = 1; which <= wanted; which++) {
          int slot = chunk->code[offset + which];
          if (slot <= 0 || slot >= IR_MAX_STACK || map[slot] < 0) return -1;
          if (top >= IR_MAX_STACK) return -1;
          map[top++] = map[slot];
        }
        if (opcode == OP_GET_LOCAL_CONST) {
          int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
          if (index < 0 || index >= chunk->constants.count) return -1;
          Value constant = chunk->constants.values[index];
          IrType type = IS_NUMBER(constant) ? IR_TYPE_NUMBER : IS_BOOL(constant) ? IR_TYPE_BOOL : IR_TYPE_UNKNOWN;
          if (top >= IR_MAX_STACK) return -1;
          int result = csIrNewRegister(ir, type);
          IrInst *inst = csIrAppend(block, IR_CONST, line);
          inst->result = result;
          inst->constant = constant;
          inst->type = type;
          map[top++] = result;
        }
        break;
      }

      case OP_SET_LOCAL:
      case OP_SET_LOCAL_POP: {
        int slot = chunk->code[offset + 1];
        if (slot <= 0 || slot >= IR_MAX_STACK) return -1;
        if (top <= floor) return -1;
        map[slot] = map[top - 1];
        if (opcode == OP_SET_LOCAL_POP) top--;
        break;
      }

      case OP_INC_LOCAL:
      case OP_DEC_LOCAL: {
        int slot = chunk->code[offset + 1];
        if (slot <= 0 || slot >= IR_MAX_STACK) return -1;
        if (map[slot] < 0 || ir->registerTypes[map[slot]] != IR_TYPE_NUMBER) return -1;

        int one = csIrNewRegister(ir, IR_TYPE_NUMBER);
        IrInst *constant = csIrAppend(block, IR_CONST, line);
        constant->result = one;
        constant->constant = NUMBER_VAL(1);
        constant->type = IR_TYPE_NUMBER;

        int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
        IrInst *inst = csIrAppend(block, opcode == OP_INC_LOCAL ? IR_ADD : IR_SUB, line);
        inst->result = result;
        inst->a = map[slot];
        inst->b = one;
        inst->type = IR_TYPE_NUMBER;
        map[slot] = result;
        break;
      }

      case OP_DUP: {
        if (top <= floor || top >= IR_MAX_STACK) return -1;
        map[top] = map[top - 1];
        top++;
        break;
      }

      case OP_POP:
        if (top <= floor) return -1;
        top--;
        break;

      case OP_NEGATE: {
        int result = csIrNewRegister(ir, IR_TYPE_NUMBER);
        IrInst *inst = csIrAppend(block, IR_NEG, line);
        inst->result = result;
        inst->a = map[top - 1];
        inst->type = IR_TYPE_NUMBER;
        map[top - 1] = result;
        break;
      }

      default: {
        IrOp op;
        IrType type = IR_TYPE_NUMBER;
        switch (opcode) {
          case OP_ADD:
          case OP_ADD_NUM: op = IR_ADD; break;
          case OP_SUBTRACT: op = IR_SUB; break;
          case OP_MULTIPLY: op = IR_MUL; break;
          case OP_DIVIDE: op = IR_DIV; break;
          case OP_MODULO: op = IR_MOD; break;
          case OP_LESS:
            op = IR_LT;
            type = IR_TYPE_BOOL;
            break;
          case OP_LESS_EQUAL:
            op = IR_LE;
            type = IR_TYPE_BOOL;
            break;
          case OP_GREATER:
            op = IR_GT;
            type = IR_TYPE_BOOL;
            break;
          case OP_GREATER_EQUAL:
            op = IR_GE;
            type = IR_TYPE_BOOL;
            break;
          case OP_EQUAL:
            op = IR_EQ;
            type = IR_TYPE_BOOL;
            break;
          case OP_NOT_EQUAL:
            op = IR_NE;
            type = IR_TYPE_BOOL;
            break;
          default: return -1;
        }
        if (top - 2 < floor) return -1;
        int result = csIrNewRegister(ir, type);
        IrInst *inst = csIrAppend(block, op, line);
        inst->result = result;
        inst->a = map[top - 2];
        inst->b = map[top - 1];
        inst->type = type;
        top--;
        map[top - 1] = result;
        break;
      }
    }

    if (++emitted > IR_INLINE_MAX_OPS) return -1;
    offset = next;
  }
  return -1;
}
