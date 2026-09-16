/* ir_print.c — the IR in readable form, and where its typing stops.
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

static const char *opName(IrOp op);

/* ---- printing ---------------------------------------------------------- */

const char *csIrOpName(IrOp op) {
  return opName(op);
}

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
    case IR_LOAD_PROPERTY: return "loadprop";
    case IR_STORE_PROPERTY: return "storeprop";
    case IR_ADD_PROPERTY: return "addprop";
    case IR_EXIT: return "exit";
    case IR_LOAD_GLOBAL: return "loadg";
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
  printf("  ir for %s: %d block%s, %d registers, %d slots\n", ir->source->name != NULL ? ir->source->name->chars : "<top level>", ir->blockCount,
         ir->blockCount == 1 ? "" : "s", ir->registerCount, ir->slotCount);

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
        case IR_STORE_PROPERTY: printf(" slot%d.%d, r%d", inst->a, inst->c, inst->b); break;
        case IR_ADD_PROPERTY: printf(" slot%d.+%d, r%d", inst->a, inst->c, inst->b); break;
        case IR_NEG: printf(" r%d", inst->a); break;
        default: printf(" r%d, r%d", inst->a, inst->b); break;
      }
      printf("\n");
    }
  }
}

/* Where the typing stops, which is the question the tiering report could not
 * answer: a function is refused for not being fully typed, and until now
 * nothing said which value it could not prove. The answer is always a pair —
 * the arithmetic that wanted a number, and the instruction that produced the
 * operand it could not have. */
bool csIrFirstUntyped(const IrFunction *ir, const char **producer, const char **consumer) {
  *producer = NULL;
  *consumer = NULL;

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
        case IR_GE: break;
        default: continue;
      }

      int wanted[2] = {inst->a, inst->b};
      for (int k = 0; k < 2; k++) {
        if (ir->registerTypes[wanted[k]] == IR_TYPE_NUMBER) continue;

        /* Whatever wrote that register, searched from the start because the
         * IR is small and this runs once, for a report. */
        *consumer = opName(inst->op);
        *producer = "unknown";
        for (int pb = 0; pb < ir->blockCount; pb++) {
          for (int pi = 0; pi < ir->blocks[pb].count; pi++) {
            const IrInst *candidate = &ir->blocks[pb].instructions[pi];
            if (candidate->result != wanted[k]) continue;
            *producer = opName(candidate->op);
          }
        }
        return true;
      }
    }
  }

  /* Nothing to compile rather than something unproved: the body reached
   * whatever the lowering could not express and became an exit, so there is no
   * arithmetic left to be worth compiling. Naming that opcode is what turns
   * the refusal into a work item. */
  if (ir->firstExitOn != NULL) {
    *consumer = "nothing";
    *producer = ir->firstExitOn;
    return true;
  }
  return false;
}
