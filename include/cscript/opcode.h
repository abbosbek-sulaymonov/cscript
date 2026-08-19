/* opcode.h — the VM instruction set.
 *
 * Operands are inline bytes following the opcode. Keep this list and the
 * disassembler in debug.c in step; every opcode needs an entry in both.
 */
#ifndef CSCRIPT_OPCODE_H
#define CSCRIPT_OPCODE_H

typedef enum {
  OP_CONSTANT,      /* [const]        push constants[const]                  */
  OP_NULL,          /*                push null                              */
  OP_UNDEFINED,     /*                push undefined                         */
  OP_TRUE,
  OP_FALSE,

  OP_POP,           /*                discard top                            */
  OP_POP_N,         /* [count]        discard `count` values                 */
  OP_DUP,           /*                duplicate top                          */

  /* Variables. Locals live in stack slots resolved at compile time; globals go
   * through a hash lookup, which is why locals are the fast path. */
  OP_DEFINE_GLOBAL, /* [const]        pop, bind constants[const] -> value     */
  OP_DEFINE_CONST,  /* [const]        same, and refuse later assignment       */
  OP_GET_GLOBAL,    /* [const]        push the global named constants[const]  */
  OP_SET_GLOBAL,    /* [const]        assign top to that global, leaving it   */
  OP_GET_LOCAL,     /* [slot]         push stack[slot]                        */
  OP_SET_LOCAL,     /* [slot]         stack[slot] = top, leaving it           */

  OP_GET_PROPERTY,  /* [const]        pop object, push its named property     */

  OP_CALL,          /* [argc]         call the value below the arguments      */

  OP_ADD,           /* numeric add, or string concat if either side is a string */
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_MODULO,
  OP_NEGATE,

  OP_NOT,           /* logical negation, using JS truthiness */
  OP_TYPEOF,

  OP_EQUAL,         /* === — CScript has no coercing equality */
  OP_NOT_EQUAL,     /* !== */
  OP_GREATER,
  OP_GREATER_EQUAL,
  OP_LESS,
  OP_LESS_EQUAL,

  OP_JUMP,          /* [hi][lo]       jump forward unconditionally            */
  OP_JUMP_IF_FALSE, /* [hi][lo]       jump forward if top is falsy, leaving it */
  OP_JUMP_IF_TRUE,  /* [hi][lo]       jump forward if top is truthy, leaving it */
  OP_POP_JUMP_IF_FALSE, /* [hi][lo]   pop, then jump forward if it was falsy  */
  OP_LOOP,          /* [hi][lo]       jump backward                           */

  OP_RETURN,
} OpCode;

#endif /* CSCRIPT_OPCODE_H */
