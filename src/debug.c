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
  printf("%-22s %4d '", name, constant);
  csValuePrint(chunk->constants.values[constant]);
  printf("'\n");
  return offset + 2;
}

static int byteInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t operand = chunk->code[offset + 1];
  printf("%-22s %4d\n", name, operand);
  return offset + 2;
}

static int jumpInstruction(const char *name, int sign, const Chunk *chunk, int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
  jump |= chunk->code[offset + 2];
  printf("%-22s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
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
    case OP_POP_N:             return byteInstruction("OP_POP_N", chunk, offset);
    case OP_DUP:               return simpleInstruction("OP_DUP", offset);
    case OP_DEFINE_GLOBAL:     return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
    case OP_DEFINE_CONST:      return constantInstruction("OP_DEFINE_CONST", chunk, offset);
    case OP_GET_GLOBAL:        return constantInstruction("OP_GET_GLOBAL", chunk, offset);
    case OP_SET_GLOBAL:        return constantInstruction("OP_SET_GLOBAL", chunk, offset);
    case OP_GET_LOCAL:         return byteInstruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:         return byteInstruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_PROPERTY:      return constantInstruction("OP_GET_PROPERTY", chunk, offset);
    case OP_CALL:              return byteInstruction("OP_CALL", chunk, offset);
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
    case OP_GREATER:           return simpleInstruction("OP_GREATER", offset);
    case OP_GREATER_EQUAL:     return simpleInstruction("OP_GREATER_EQUAL", offset);
    case OP_LESS:              return simpleInstruction("OP_LESS", offset);
    case OP_LESS_EQUAL:        return simpleInstruction("OP_LESS_EQUAL", offset);
    case OP_JUMP:              return jumpInstruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE:     return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_JUMP_IF_TRUE:      return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
    case OP_POP_JUMP_IF_FALSE: return jumpInstruction("OP_POP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:              return jumpInstruction("OP_LOOP", -1, chunk, offset);
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
    case AST_IDENTIFIER:
      printf("Identifier %.*s\n", node->as.identifier.length, node->as.identifier.name);
      break;
    case AST_ASSIGN:
      printf("Assign\n");
      printNode(node->as.assign.target, depth + 1);
      printNode(node->as.assign.value, depth + 1);
      break;
    case AST_UPDATE:
      printf("Update %s%s\n", node->as.update.isIncrement ? "++" : "--",
             node->as.update.isPrefix ? " (prefix)" : " (postfix)");
      printNode(node->as.update.target, depth + 1);
      break;
    case AST_PROPERTY:
      printf("Property .%.*s\n", node->as.property.length, node->as.property.name);
      printNode(node->as.property.object, depth + 1);
      break;
    case AST_CALL:
      printf("Call (%d argument%s)\n", node->as.call.argCount,
             node->as.call.argCount == 1 ? "" : "s");
      printNode(node->as.call.callee, depth + 1);
      for (int i = 0; i < node->as.call.argCount; i++) {
        printNode(node->as.call.arguments[i], depth + 1);
      }
      break;
    case AST_EXPRESSION_STMT:
      printf("ExpressionStmt\n");
      printNode(node->as.expression, depth + 1);
      break;
    case AST_VAR_DECL:
      printf("%s %.*s\n", node->as.varDecl.isConst ? "Const" : "Let",
             node->as.varDecl.length, node->as.varDecl.name);
      if (node->as.varDecl.initializer != NULL) {
        printNode(node->as.varDecl.initializer, depth + 1);
      }
      break;
    case AST_BLOCK:
      printf("Block (%d statement%s)\n", node->as.block.count,
             node->as.block.count == 1 ? "" : "s");
      for (int i = 0; i < node->as.block.count; i++) {
        printNode(node->as.block.statements[i], depth + 1);
      }
      break;
    case AST_IF_STMT:
      printf("If\n");
      printNode(node->as.ifStmt.condition, depth + 1);
      printNode(node->as.ifStmt.thenBranch, depth + 1);
      if (node->as.ifStmt.elseBranch != NULL) {
        printNode(node->as.ifStmt.elseBranch, depth + 1);
      }
      break;
    case AST_WHILE_STMT:
      printf("While\n");
      printNode(node->as.whileStmt.condition, depth + 1);
      printNode(node->as.whileStmt.body, depth + 1);
      break;
    case AST_FOR_STMT:
      printf("For\n");
      printNode(node->as.forStmt.initializer, depth + 1);
      printNode(node->as.forStmt.condition, depth + 1);
      printNode(node->as.forStmt.increment, depth + 1);
      printNode(node->as.forStmt.body, depth + 1);
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
