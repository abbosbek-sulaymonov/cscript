/* ir_interpret.c — running the lowered form, to check it against the bytecode.
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

/* ---- running it -------------------------------------------------------- */

/* An interpreter for the IR.
 *
 * This exists to be thrown away. Its purpose is to answer one question before
 * a code generator is written: does the lowering mean the same thing as the
 * bytecode it came from? Running both and comparing is a far cheaper way to
 * find a mistranslation than finding it in machine code, where the symptom is
 * a wrong number and the cause is three layers down.
 */
#define IR_INTERPRET_MAX_SLOTS 256
#define IR_MAX_STEPS 200000000

bool csIrInterpret(const IrFunction *ir, const Value *args, int argCount, Value *out) {
  if (ir->slotCount > IR_INTERPRET_MAX_SLOTS) return false;

  Value slots[IR_INTERPRET_MAX_SLOTS];
  for (int i = 0; i < ir->slotCount; i++) slots[i] = UNDEFINED_VAL;
  /* Slot 0 is the callee, as in a real frame; the arguments follow it. */
  for (int i = 0; i < argCount && i + 1 < ir->slotCount; i++) slots[i + 1] = args[i];

  Value *registers = (Value *)malloc(sizeof(Value) * (size_t)(ir->registerCount + 1));
  for (int i = 0; i < ir->registerCount; i++) registers[i] = UNDEFINED_VAL;

  /* Both arrays are the collector's business now that an instruction here can
   * allocate: the frame holds the objects, and a register holds one for as
   * long as it is between a load and its use. */
  int savedRanges = vm.jitRootRanges;
  vm.jitRoots[0].values = slots;
  vm.jitRoots[0].count = ir->slotCount;
  vm.jitRoots[1].values = registers;
  vm.jitRoots[1].count = ir->registerCount;
  vm.jitRootRanges = 2;

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
      if ((inst->op == IR_LOAD_LOCAL || inst->op == IR_STORE_LOCAL) && (inst->a < 0 || inst->a >= ir->slotCount)) {
        goto done;
      }
      /* Every register operand, not only the result. A negative one means the
       * lowering referred to a value it never produced. */
      if (inst->op == IR_STORE_LOCAL && (inst->b < 0 || inst->b >= ir->registerCount)) {
        goto done;
      }
      if (inst->op == IR_EXIT) goto done;
      if (inst->op == IR_STORE_GLOBAL && (inst->b < 0 || inst->b >= ir->registerCount)) {
        goto done;
      }
      if (inst->op != IR_CONST && inst->op != IR_LOAD_LOCAL && inst->op != IR_STORE_LOCAL && inst->op != IR_JUMP && inst->op != IR_LOAD_GLOBAL &&
          inst->op != IR_STORE_GLOBAL) {
        if (inst->a < 0 || inst->a >= ir->registerCount) goto done;
        if (inst->op != IR_NEG && inst->op != IR_RETURN && inst->op != IR_BRANCH && (inst->b < 0 || inst->b >= ir->registerCount)) {
          goto done;
        }
      }

      switch (inst->op) {
        case IR_CONST: registers[inst->result] = inst->constant; break;
        case IR_LOAD_LOCAL: registers[inst->result] = slots[inst->a]; break;

        case IR_STORE_PROPERTY: {
          ObjObject *target = AS_OBJECT(slots[inst->a]);
          target->as.slots.values[inst->c] = registers[inst->b];
          break;
        }

        case IR_NEW_OBJECT: {
          /* The same call compiled code makes, from the same frame. The IR
           * interpreter runs on its own slot array, which is a root for the
           * same reason and by the same means. */
          if (inst->b < 0 || inst->b >= ir->literalCount) goto done;
          csJitBuildObject(slots, inst->a, &ir->literals[inst->b]);
          break;
        }

        case IR_ADD_PROPERTY: {
          /* The value first and the layout second: the collector sizes its
           * walk of the slots from the shape, so the other order would show it
           * a slot the shape counts and nothing has written. Room for it was
           * proved at entry. */
          ObjObject *target = AS_OBJECT(slots[inst->a]);
          target->as.slots.values[inst->c] = registers[inst->b];
          target->shape = (Shape *)AS_OBJ(inst->constant);
          break;
        }

        case IR_LOAD_PROPERTY: {
          /* Unguarded, because the entry check already proved the layout and
           * the slot is one this function never writes. */
          ObjObject *object = AS_OBJECT(slots[inst->a]);
          registers[inst->result] = object->as.slots.values[inst->b];
          break;
        }
        case IR_STORE_LOCAL: slots[inst->a] = registers[inst->b]; break;

        case IR_ADD:
          /* The one operator that is not arithmetic when a string is involved.
           * The lowering records which case it is in the result type. */
          if (inst->type != IR_TYPE_NUMBER && (!IS_NUMBER(registers[inst->a]) || !IS_NUMBER(registers[inst->b]))) {
            goto done; /* leave it to the bytecode VM */
          }
          registers[inst->result] = NUMBER_VAL(AS_NUMBER(registers[inst->a]) + AS_NUMBER(registers[inst->b]));
          break;

        case IR_SUB: registers[inst->result] = NUMBER_VAL(AS_NUMBER(registers[inst->a]) - AS_NUMBER(registers[inst->b])); break;
        case IR_MUL: registers[inst->result] = NUMBER_VAL(AS_NUMBER(registers[inst->a]) * AS_NUMBER(registers[inst->b])); break;
        case IR_DIV: registers[inst->result] = NUMBER_VAL(AS_NUMBER(registers[inst->a]) / AS_NUMBER(registers[inst->b])); break;
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
        case IR_NEG: registers[inst->result] = NUMBER_VAL(-AS_NUMBER(registers[inst->a])); break;

        case IR_LT: registers[inst->result] = BOOL_VAL(AS_NUMBER(registers[inst->a]) < AS_NUMBER(registers[inst->b])); break;
        case IR_LE: registers[inst->result] = BOOL_VAL(AS_NUMBER(registers[inst->a]) <= AS_NUMBER(registers[inst->b])); break;
        case IR_GT: registers[inst->result] = BOOL_VAL(AS_NUMBER(registers[inst->a]) > AS_NUMBER(registers[inst->b])); break;
        case IR_GE: registers[inst->result] = BOOL_VAL(AS_NUMBER(registers[inst->a]) >= AS_NUMBER(registers[inst->b])); break;
        case IR_EQ: registers[inst->result] = BOOL_VAL(csValuesStrictEqual(registers[inst->a], registers[inst->b])); break;
        case IR_NE: registers[inst->result] = BOOL_VAL(!csValuesStrictEqual(registers[inst->a], registers[inst->b])); break;

        case IR_JUMP: nextBlock = inst->a; goto blockDone;
        case IR_BRANCH: nextBlock = AS_BOOL(registers[inst->a]) ? inst->b : inst->c; goto blockDone;
        case IR_LOAD_GLOBAL: {
          Value key = ir->source->chunk.constants.values[inst->a];
          Value held;
          if (!IS_STRING(key) || ir->source->module == NULL || !csTableGet(&ir->source->module->globals, AS_STRING(key), &held)) {
            goto done;
          }
          registers[inst->result] = held;
          break;
        }

        case IR_STORE_GLOBAL: {
          Value key = ir->source->chunk.constants.values[inst->a];
          if (!IS_STRING(key) || ir->source->module == NULL) goto done;
          csTableSet(&ir->source->module->globals, AS_STRING(key), registers[inst->b]);
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
  vm.jitRootRanges = savedRanges;
  free(registers);
  return ok;
}
