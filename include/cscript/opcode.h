/* opcode.h — the VM instruction set.
 *
 * Operands are inline bytes following the opcode. Keep this list and the
 * disassembler in debug.c in step; every opcode needs an entry in both.
 */
#ifndef CSCRIPT_OPCODE_H
#define CSCRIPT_OPCODE_H

typedef enum {
  OP_CONSTANT,      /* [const_index]  push constants[index] */
  OP_NULL,          /*                push null             */
  OP_UNDEFINED,     /*                push undefined        */
  OP_TRUE,          /*                push true             */
  OP_FALSE,         /*                push false            */

  OP_POP,           /*                discard top           */

  OP_ADD,           /* numeric add, or string concat if either side is a string */
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_MODULO,
  OP_NEGATE,

  OP_NOT,           /* logical negation, using JS truthiness */
  OP_TYPEOF,

  OP_EQUAL,         /* ==  */
  OP_NOT_EQUAL,     /* !=  */
  OP_STRICT_EQUAL,  /* === */
  OP_STRICT_NOT_EQUAL, /* !== */
  OP_GREATER,
  OP_GREATER_EQUAL,
  OP_LESS,
  OP_LESS_EQUAL,

  OP_JUMP_IF_FALSE, /* [hi][lo]  jump forward if top is falsy, leaving it     */
  OP_JUMP_IF_TRUE,  /* [hi][lo]  jump forward if top is truthy, leaving it    */

  OP_PRINT,
  OP_RETURN,
} OpCode;

#endif /* CSCRIPT_OPCODE_H */
