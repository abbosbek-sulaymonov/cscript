/* opcode.h — the VM instruction set.
 *
 * The list is declared once, as an X-macro, and used to generate the enum here
 * and the computed-goto dispatch table in vm.c. Keeping them in one place is
 * what makes it impossible for the table to drift out of step with the enum —
 * a mismatch there would jump to the wrong handler rather than fail to build.
 *
 * Operands are inline bytes following the opcode. Adding an opcode still means
 * adding a case to the disassembler in debug.c, which the compiler will not
 * catch for you.
 */
#ifndef CSCRIPT_OPCODE_H
#define CSCRIPT_OPCODE_H

/* clang-format off */
#define CS_OPCODE_LIST(X)                                                     \
  X(OP_CONSTANT)          /* [const]  push constants[const]                */ \
  X(OP_NULL)              /*          push null                            */ \
  X(OP_UNDEFINED)         /*          push undefined                       */ \
  X(OP_TRUE)                                                                  \
  X(OP_FALSE)                                                                 \
                                                                              \
  X(OP_POP)               /*          discard top                          */ \
  X(OP_POP_N)             /* [count]  discard `count` values               */ \
  X(OP_DUP)               /*          duplicate top                        */ \
                                                                              \
  /* Variables. Locals live in stack slots resolved at compile time; globals  \
   * go through a hash lookup, which is why locals are the fast path. */       \
  X(OP_DEFINE_GLOBAL)     /* [const]  pop, bind constants[const] -> value   */ \
  X(OP_DEFINE_CONST)      /* [const]  same, and refuse later assignment     */ \
  X(OP_GET_GLOBAL)        /* [const]  push the global named constants[const]*/ \
  X(OP_SET_GLOBAL)        /* [const]  assign top to that global, leaving it */ \
  X(OP_GET_LOCAL)         /* [slot]   push stack[slot]                      */ \
  X(OP_SET_LOCAL)         /* [slot]   stack[slot] = top, leaving it         */ \
  /* In-place ++/-- on a local whose result is discarded. One instruction     \
   * instead of the six a general update needs, and no stack traffic. */       \
  X(OP_INC_LOCAL)         /* [slot]   stack[slot] += 1                      */ \
  X(OP_DEC_LOCAL)         /* [slot]   stack[slot] -= 1                      */ \
                                                                              \
  X(OP_GET_PROPERTY)      /* [const]  pop object, push its named property   */ \
  X(OP_CALL)              /* [argc]   call the value below the arguments    */ \
                                                                              \
  /* Closures. OP_CLOSURE is followed by one (isLocal, index) pair per         \
   * upvalue, describing where each capture comes from. */                     \
  X(OP_CLOSURE)           /* [const] then 2 bytes per upvalue               */ \
  X(OP_GET_UPVALUE)       /* [slot]   push the captured variable            */ \
  X(OP_SET_UPVALUE)       /* [slot]   assign to it, leaving the value       */ \
  X(OP_CLOSE_UPVALUE)     /*          move the top local off the stack      */ \
                                                                              \
  X(OP_ADD)               /* numeric add, or string concat if either is a string */ \
  /* Emitted only where the type checker proved both operands are numbers, so \
   * it needs no type dispatch at all. This is the first place a type          \
   * annotation changes the code that runs rather than only what compiles. */  \
  X(OP_ADD_NUM)           /* both operands statically number                */ \
  X(OP_SUBTRACT)                                                              \
  X(OP_MULTIPLY)                                                              \
  X(OP_DIVIDE)                                                                \
  X(OP_MODULO)                                                                \
  X(OP_NEGATE)                                                                \
                                                                              \
  X(OP_NOT)               /* logical negation, using JS truthiness         */ \
  X(OP_TYPEOF)                                                                \
                                                                              \
  X(OP_EQUAL)             /* === — CScript has no coercing equality        */ \
  X(OP_NOT_EQUAL)         /* !==                                           */ \
  X(OP_GREATER)                                                               \
  X(OP_GREATER_EQUAL)                                                         \
  X(OP_LESS)                                                                  \
  X(OP_LESS_EQUAL)                                                            \
                                                                              \
  X(OP_JUMP)              /* [hi][lo] jump forward unconditionally         */ \
  X(OP_JUMP_IF_FALSE)     /* [hi][lo] jump if top is falsy, leaving it      */ \
  X(OP_JUMP_IF_TRUE)      /* [hi][lo] jump if top is truthy, leaving it     */ \
  X(OP_POP_JUMP_IF_FALSE) /* [hi][lo] pop, then jump if it was falsy        */ \
  X(OP_LOOP)              /* [hi][lo] jump backward                         */ \
                                                                              \
  X(OP_RETURN)
/* clang-format on */

typedef enum {
#define CS_DECLARE_OPCODE(name) name,
  CS_OPCODE_LIST(CS_DECLARE_OPCODE)
#undef CS_DECLARE_OPCODE
      OP_COUNT /* number of opcodes; not itself an instruction */
} OpCode;

#endif /* CSCRIPT_OPCODE_H */
