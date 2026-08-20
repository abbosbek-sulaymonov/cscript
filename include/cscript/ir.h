/* ir.h — a typed intermediate representation, between bytecode and machine
 * code.
 *
 * The bytecode is a stack machine: an instruction's operands are wherever the
 * previous ones left them. Machine code needs the opposite — operands named
 * explicitly, so a register allocator can decide where they live. Lowering to
 * this form is the step that makes that possible, and it is also where the
 * types the checker proved stop being advisory and start being structural.
 *
 * **Not SSA, deliberately.** Locals stay in numbered slots and only
 * expression temporaries become virtual registers. Full SSA with phi nodes
 * would be needed for the optimisations that reason across a loop; nothing
 * here does that yet, and building the dominance machinery before there is a
 * backend to consume it would be writing for an imagined future. A slot model
 * is what a first code generator wants and is far easier to verify.
 *
 * **Types come from the bytecode, not from a second analysis.** OP_ADD_NUM is
 * emitted exactly where the checker proved both operands are numbers, and the
 * arithmetic opcodes require numbers by definition. So the lowering already
 * knows more than a JavaScript engine could infer without watching the program
 * run — which is the whole argument for this design over a speculative one.
 */
#ifndef CSCRIPT_IR_H
#define CSCRIPT_IR_H

#include "cscript/common.h"
#include "cscript/object.h"
#include "cscript/value.h"

typedef enum {
  IR_TYPE_UNKNOWN, /* a boxed Value; nothing is known about it */
  IR_TYPE_NUMBER,  /* proved: can live unboxed in a floating-point register */
  IR_TYPE_BOOL,
} IrType;

typedef enum {
  IR_CONST,       /* a := constant                          */
  IR_LOAD_LOCAL,  /* a := slot[b]                           */
  IR_STORE_LOCAL, /* slot[a] := b                           */

  IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
  IR_NEG,

  IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE,

  IR_JUMP,        /* -> block a                             */
  IR_BRANCH,      /* if a then block b else block c         */
  IR_RETURN,      /* return a                               */
} IrOp;

typedef struct {
  IrOp op;
  int result; /* virtual register written, or -1 */
  int a, b, c;
  Value constant; /* IR_CONST only */
  IrType type;    /* of `result` */
  int line;
} IrInst;

typedef struct {
  IrInst *instructions;
  int count;
  int capacity;
  int bytecodeStart; /* where this block began, for mapping back */
} IrBlock;

typedef struct {
  ObjFunction *source;

  IrBlock *blocks;
  int blockCount;
  int blockCapacity;

  int registerCount;
  IrType *registerTypes;
  int registerCapacity;

  int slotCount; /* locals, which stay in slots rather than registers */
} IrFunction;

/* Lowers a function's bytecode.
 *
 * Returns NULL and writes why into `reason` when the function holds something
 * this form cannot express yet — which is most of the language. The refusal is
 * the useful part: it says exactly how far the lowering reaches. */
IrFunction *csIrLower(ObjFunction *function, const char **reason);

void csIrFree(IrFunction *ir);

/* Prints the IR, for `make jit`. */
void csIrPrint(const IrFunction *ir);

/* Whether every value in the IR is provably a number or a boolean.
 *
 * This is the gate on actually *running* the lowered form. A function with an
 * unknown-typed value in it may still have lowered — the shape of the control
 * flow is fine — but the IR does not model what that value is, so running it
 * would be a guess. It is also exactly the class of function a first code
 * generator targets: no boxes, no guards, nothing to deoptimise to.
 *
 * Lowering and running are deliberately separate questions. The first says how
 * far the translation reaches; the second says how far it can be trusted. */
bool csIrIsFullyTyped(const IrFunction *ir);

/* Runs the IR directly, so the lowering can be checked against the bytecode VM
 * before any machine code exists. `args` are the arguments; the result goes to
 * `out`. Returns false when the IR runs off the end of what it supports. */
bool csIrInterpret(const IrFunction *ir, const Value *args, int argCount, Value *out);

#endif /* CSCRIPT_IR_H */
