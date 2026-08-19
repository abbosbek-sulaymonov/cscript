#include <stdio.h>

#include "cscript/debug.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/value.h"

static int simpleInstruction(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

static int constantInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t constant = chunk->code[offset + 1];
  printf("%-18s %4d '", name, constant);
  csValuePrint(chunk->constants.values[constant]);
  printf("'\n");
  return offset + 2;
}

static int jumpInstruction(const char *name, int sign, const Chunk *chunk, int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
  jump |= chunk->code[offset + 2];
  printf("%-18s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
}

int csDisassembleInstruction(const Chunk *chunk, int offset) {
  printf("%04d ", offset);

  /* Repeat the line number only when it changes, so runs read as blocks. */
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines[offset]);
  }

  uint8_t instruction = chunk->code[offset];
  switch (instruction) {
    case OP_CONSTANT:          return constantInstruction("OP_CONSTANT", chunk, offset);
    case OP_NULL:              return simpleInstruction("OP_NULL", offset);
    case OP_UNDEFINED:         return simpleInstruction("OP_UNDEFINED", offset);
    case OP_TRUE:              return simpleInstruction("OP_TRUE", offset);
    case OP_FALSE:             return simpleInstruction("OP_FALSE", offset);
    case OP_POP:               return simpleInstruction("OP_POP", offset);
    case OP_ADD:               return simpleInstruction("OP_ADD", offset);
    case OP_SUBTRACT:          return simpleInstruction("OP_SUBTRACT", offset);
    case OP_MULTIPLY:          return simpleInstruction("OP_MULTIPLY", offset);
    case OP_DIVIDE:            return simpleInstruction("OP_DIVIDE", offset);
    case OP_MODULO:            return simpleInstruction("OP_MODULO", offset);
    case OP_NEGATE:            return simpleInstruction("OP_NEGATE", offset);
    case OP_NOT:               return simpleInstruction("OP_NOT", offset);
    case OP_TYPEOF:            return simpleInstruction("OP_TYPEOF", offset);
    case OP_EQUAL:             return simpleInstruction("OP_EQUAL", offset);
    case OP_NOT_EQUAL:         return simpleInstruction("OP_NOT_EQUAL", offset);
    case OP_STRICT_EQUAL:      return simpleInstruction("OP_STRICT_EQUAL", offset);
    case OP_STRICT_NOT_EQUAL:  return simpleInstruction("OP_STRICT_NOT_EQUAL", offset);
    case OP_GREATER:           return simpleInstruction("OP_GREATER", offset);
    case OP_GREATER_EQUAL:     return simpleInstruction("OP_GREATER_EQUAL", offset);
    case OP_LESS:              return simpleInstruction("OP_LESS", offset);
    case OP_LESS_EQUAL:        return simpleInstruction("OP_LESS_EQUAL", offset);
    case OP_JUMP_IF_FALSE:     return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_JUMP_IF_TRUE:      return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
    case OP_PRINT:             return simpleInstruction("OP_PRINT", offset);
    case OP_RETURN:            return simpleInstruction("OP_RETURN", offset);
    default:
      printf("unknown opcode %d\n", instruction);
      return offset + 1;
  }
}

void csDisassembleChunk(const Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);
  for (int offset = 0; offset < chunk->count;) {
    offset = csDisassembleInstruction(chunk, offset);
  }
  printf("\n");
}

static void indent(int depth) {
  for (int i = 0; i < depth; i++) printf("  ");
}

static void printNode(const AstNode *node, int depth) {
  if (node == NULL) {
    indent(depth);
    printf("<null>\n");
    return;
  }

  indent(depth);
  switch (node->type) {
    case AST_NUMBER_LITERAL:
      printf("Number ");
      csValuePrint(NUMBER_VAL(node->as.number));
      printf("\n");
      break;
    case AST_STRING_LITERAL:
      printf("String \"%.*s\"\n", node->as.string.length, node->as.string.chars);
      break;
    case AST_BOOL_LITERAL:
      printf("Bool %s\n", node->as.boolean ? "true" : "false");
      break;
    case AST_NULL_LITERAL:
      printf("Null\n");
      break;
    case AST_UNDEFINED_LITERAL:
      printf("Undefined\n");
      break;
    case AST_UNARY:
      printf("Unary %s\n", csUnaryOpName(node->as.unary.op));
      printNode(node->as.unary.operand, depth + 1);
      break;
    case AST_BINARY:
      printf("Binary %s\n", csBinaryOpName(node->as.binary.op));
      printNode(node->as.binary.left, depth + 1);
      printNode(node->as.binary.right, depth + 1);
      break;
    case AST_LOGICAL:
      printf("Logical %s\n", csLogicalOpName(node->as.logical.op));
      printNode(node->as.logical.left, depth + 1);
      printNode(node->as.logical.right, depth + 1);
      break;
    case AST_GROUPING:
      printf("Grouping\n");
      printNode(node->as.grouping, depth + 1);
      break;
    case AST_EXPRESSION_STMT:
      printf("ExpressionStmt\n");
      printNode(node->as.expression, depth + 1);
      break;
    case AST_PRINT_STMT:
      printf("PrintStmt\n");
      printNode(node->as.expression, depth + 1);
      break;
    case AST_PROGRAM:
      printf("Program (%d statement%s)\n", node->as.program.count,
             node->as.program.count == 1 ? "" : "s");
      for (int i = 0; i < node->as.program.count; i++) {
        printNode(node->as.program.statements[i], depth + 1);
      }
      break;
  }
}

void csAstPrint(const AstNode *node) {
  printf("== ast ==\n");
  printNode(node, 0);
  printf("\n");
}
