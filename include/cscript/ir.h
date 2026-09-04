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

  /* A module-level binding. `a` is the index of its name in the constant
   * pool, which is how the address is found again at compile time.
   *
   * Only ever emitted where the global already holds a number: what makes
   * that safe is not a guess but the language — the checker refuses to assign
   * a different type to a declared binding, and nothing inside a compiled
   * region can call anything, so the only writer is the compiled code itself
   * and every store it makes is proved numeric. */
  IR_LOAD_GLOBAL,  /* a := globals[name a]                  */
  IR_STORE_GLOBAL, /* globals[name a] := b                  */

  /* `result := slots[a].<property b>`, where `b` is an index into the object's
   * own storage rather than a name.
   *
   * There is no guard on it, and that is the whole design. A guard in the body
   * would be an exit, and a function with an exit can only be entered where a
   * frame already exists — so guarding here would have bought property reads
   * at the price of never answering a call with them, which is the only place
   * they would pay. Instead the shape the read was lowered against is recorded
   * as an entry assumption and checked once, before the body starts: see
   * IrEntryShape. That works because the object comes from a frame slot the
   * function never writes, so what was true at entry is still true here. */
  IR_LOAD_PROPERTY,

  /* `slots[a].<property c> := b`, and unguarded for the same reason as the
   * load: the shape was checked once at entry and the slot it came from is one
   * the function never writes, so the storage index is still the right one.
   *
   * Nothing else has to happen, because the property already exists at that
   * index: the entry check proves the object has exactly the recorded shape,
   * and that the index is inside it. So the write cannot grow the object or
   * move it into dictionary mode, and the collector needs no barrier because
   * it is not generational.
   *
   * A store that *adds* a property is therefore not this instruction, and is
   * refused rather than compiled. That is what excludes most constructors:
   * `this.x = x; this.y = y;` on a fresh object sees a different shape at each
   * store, because each one transitions it — and one slot may only carry one
   * recorded layout. A class whose fields are declared is the exception, since
   * the field initialisers give the instance its final shape before the
   * constructor body runs. */
  IR_STORE_PROPERTY,

  IR_JUMP,        /* -> block a                             */
  IR_BRANCH,      /* if a then block b else block c         */
  IR_RETURN,      /* return a                               */
  /* Hand this frame back to the interpreter at bytecode offset `a`, with the
   * operand stack `b` values deep.
   *
   * Whole-function lowering is too coarse for a script: almost every top level
   * ends in a `console.log`, and one call was enough to refuse the loop above
   * it. An exit lets the compiler take the part it understands and leave the
   * rest where it was — which is only possible because the compiled code keeps
   * locals in the interpreter's own frame at the interpreter's own offsets, so
   * leaving is the mirror of arriving. */
  IR_EXIT,
} IrOp;

typedef struct {
  IrOp op;
  int result; /* virtual register written, or -1 */
  int a, b, c;
  Value constant; /* IR_CONST only */
  IrType type;    /* of `result` */
  int line;
} IrInst;

/* One thing the compiled body takes for granted about the frame it is given.
 *
 * Checked at entry, by both the call path and the OSR path, and the code is
 * simply not run when it does not hold — which is the cheapest possible
 * deoptimisation: nothing has happened yet, so there is nothing to undo. */
typedef struct {
  int slot;            /* the frame slot holding the object */
  Shape *shape;        /* the layout it must still have */
  int property;        /* the storage index the reads and writes use */
  /* Whether the property has to *hold* a number at entry. A read needs that —
   * its result is used as one. A write does not: it only needs the slot to be
   * where the shape says, and what was there before is about to be replaced.
   * A property both read and written keeps the stricter of the two. */
  bool expectsNumber;
} IrEntryShape;

/* A call whose body was spliced in where the call was.
 *
 * The callee was read out of a module binding, so what makes the splice sound
 * is that the binding still holds the same function when the code runs — a
 * global is not a constant, and `dist = somethingElse` between the compile and
 * the run would leave a body inlined for a function nobody is calling. Checked
 * at both entries, alongside the shapes, and the code is simply not run when
 * it no longer holds.
 *
 * The closure is held rather than the name alone because two different
 * closures can be bound to one name in turn, and identity is the question. */
typedef struct {
  Table *globals;     /* the module table the binding lives in */
  ObjString *name;    /* the binding the callee was read from */
  ObjClosure *callee; /* the exact closure whose body was spliced in */
} IrInlinedCall;

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

  /* What each slot was proved to hold. A slot known to be a number for the
   * whole function can live in a register instead of memory — which is what a
   * loop counter and an accumulator are, and why a loop body otherwise loads
   * and stores them on every iteration exactly as the interpreter does. */
  IrType *slotTypes;

  /* True when any block ends in IR_EXIT. Such a function can only be entered
   * where a real frame already exists — that is, through on-stack replacement
   * — because an exit resumes the interpreter part-way through a body that a
   * call entry has not yet begun. */
  bool hasExits;

  /* The bytecode opcode the lowering first gave up on, or NULL. What the
   * tiering report needs to say where the compiler stops, rather than only
   * that it stopped. */
  const char *firstExitOn;

  /* What IR_LOAD_PROPERTY was lowered against. Empty for a function that reads
   * no properties, which is most of them. */
  IrEntryShape *entryShapes;
  int entryShapeCount;
  int entryShapeCapacity;

  /* The callees whose bodies were spliced into this one, and the bindings they
   * were read from. Empty for a function that inlined nothing. */
  IrInlinedCall *inlined;
  int inlinedCount;
  int inlinedCapacity;

  /* How many IR instructions came from a splice rather than from this
   * function's own bytecode, so the tiering report can say what inlining is
   * doing rather than only that it happened. */
  int inlinedInstructions;
} IrFunction;

/* Lowers a function's bytecode.
 *
 * Returns NULL and writes why into `reason` when the function holds something
 * this form cannot express yet — which is most of the language. The refusal is
 * the useful part: it says exactly how far the lowering reaches. */
IrFunction *csIrLower(ObjFunction *function, const char **reason);

void csIrFree(IrFunction *ir);

/* Which of an instruction's `a` and `b` fields name virtual registers.
 *
 * They do not always. The same two fields hold a slot number, a block index, a
 * constant-pool index, a bytecode offset and an operand-stack height depending
 * on the opcode — and every pass that walked them assuming otherwise
 * introduced a bug: one marked constants as escaping, one renamed a bytecode
 * offset into a register number, one read a stack height as a value. It is
 * answered here so there is one place to be right.
 *
 * Writes -1 for a field that is not a register. */
void csIrRegisterOperands(const IrInst *inst, int *a, int *b);

/* Makes every slot's type the meet of what is stored into it, and downgrades
 * loads that claimed more. Must run before the IR is trusted: the lowering's
 * own tracking is linear, and a loop makes linear order the wrong order. */
void csIrReconcileSlotTypes(IrFunction *ir);

/* The name of an IR opcode, for diagnostics. */
const char *csIrOpName(IrOp op);

/* Why a lowered function is not fully typed: the opcode that produced the
 * first arithmetic operand nothing could prove a number, and the arithmetic
 * that wanted it. Both are written as names; `NULL` when the function *is*
 * fully typed. Returns false when it is fully typed. */
bool csIrFirstUntyped(const IrFunction *ir, const char **producer,
                      const char **consumer);

/* Do a frame's slots still match what the body was lowered to assume? Both
 * entries ask this, and neither may skip it. */
bool csIrEntryShapesHold(const IrFunction *ir, const Value *slots);

/* Do the bindings the inlined callees came from still hold those callees?
 *
 * The mirror of csIrEntryShapesHold for calls: an inlined body is only the
 * right answer while the name it was read from still means the same function.
 * Both entries ask this, and neither may skip it. */
bool csIrInlinedCalleesHold(const IrFunction *ir);

/* Keeps what the compiled body assumes alive: the shapes an entry assumption
 * names, and the closures whose bodies were spliced into it.
 *
 * They are ordinary collectable objects and a shape's transition edges are
 * weak, so nothing else would — and a freed shape whose memory came back as a
 * different one would make the check pass when it must fail. An inlined callee
 * is the same hazard with a different object: rebind the global and the only
 * reference left to the closure is the one this check compares against. */
void csIrMarkReferences(const IrFunction *ir);

/* Prints the IR, for `make jit`. */
void csIrPrint(const IrFunction *ir);

/* Forwards a value stored to a slot into the load that reads it straight back.
 *
 * The lowering deliberately mirrors the frame, storing every value and loading
 * it again, because that is the only model that survives locals which were
 * declared rather than stored. The cost is that a two-multiply function does
 * more memory traffic than the bytecode did — which is why the first compiled
 * code came out slower than the interpreter it replaced.
 *
 * This removes the round trip where it is provably redundant: a load whose
 * slot has not been written since the store that fed it reads a value already
 * in hand. Nothing moves, nothing is reordered, and the store stays — a named
 * local may be read again later, and proving otherwise is a separate analysis.
 */
void csIrForwardSlots(IrFunction *ir);

/* Removes stores to slots nothing ever reads.
 *
 * The lowering mirrors the frame, so every value pushed on the operand stack
 * is stored to the position it occupies. Most of those positions are pure
 * temporaries — a two-operand expression uses one and never reads it back —
 * and in a loop body they are the majority of the work.
 *
 * A slot that appears in no load anywhere in the function is write-only, and
 * every store to it is dead. Whole-function rather than per-block, and safe
 * because the frame belongs to the call: the compiled code makes none of its
 * own, the IR refuses any function that captures a local, and the caller reads
 * nothing but the return value. */
void csIrRemoveDeadStores(IrFunction *ir);

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
