#include <stdio.h>

#include "cscript/debug.h"
#include "cscript/object.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/value.h"

const char *csOpcodeName(OpCode opcode) {
  switch (opcode) {
#define CS_OPCODE_NAME(name) \
  case name:                 \
    return #name;
    CS_OPCODE_LIST(CS_OPCODE_NAME)
#undef CS_OPCODE_NAME
    case OP_COUNT:
      break;
  }
  return "OP_UNKNOWN";
}

static int simpleInstruction(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

/* Constant indices are two bytes, big-endian. */
static int readConstantIndex(const Chunk *chunk, int offset) {
  return (chunk->code[offset] << 8) | chunk->code[offset + 1];
}

static int constantInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  printf("%-22s %4d '", name, constant);
  csValuePrint(chunk->constants.values[constant]);
  printf("'\n");
  return offset + 3;
}

/* A constant index followed by an inline-cache index. The cache index is
 * printed because a site that is not hitting is usually easiest to find by
 * matching it back to the entry in the chunk's cache array. */
static int cachedInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  int cache = readConstantIndex(chunk, offset + 3);
  printf("%-22s %4d '", name, constant);
  csValuePrint(chunk->constants.values[constant]);
  printf("'  cache %d\n", cache);
  return offset + 5;
}

/* Two operands: a method name and an argument count. */
static int invokeInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  uint8_t argCount = chunk->code[offset + 3];
  printf("%-22s %4d '", name, argCount);
  csValuePrint(chunk->constants.values[constant]);
  printf("'\n");
  return offset + 4;
}

static int byteInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t operand = chunk->code[offset + 1];
  printf("%-22s %4d\n", name, operand);
  return offset + 2;
}

/* Two operands: a stack slot and a constant index. */
static int slotConstantInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t slot = chunk->code[offset + 1];
  int constant = readConstantIndex(chunk, offset + 2);
  printf("%-22s %4d %4d '", name, slot, constant);
  csValuePrint(chunk->constants.values[constant]);
  printf("'\n");
  return offset + 4;
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
    case OP_GET_GLOBAL:        return cachedInstruction("OP_GET_GLOBAL", chunk, offset);
    case OP_SET_GLOBAL:        return cachedInstruction("OP_SET_GLOBAL", chunk, offset);
    case OP_GET_LOCAL:         return byteInstruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:         return byteInstruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_LOCAL_CONST:   return slotConstantInstruction("OP_GET_LOCAL_CONST", chunk, offset);
    case OP_SET_LOCAL_POP:     return byteInstruction("OP_SET_LOCAL_POP", chunk, offset);
    case OP_SET_GLOBAL_POP:    return cachedInstruction("OP_SET_GLOBAL_POP", chunk, offset);
    case OP_INC_LOCAL:         return byteInstruction("OP_INC_LOCAL", chunk, offset);
    case OP_DEC_LOCAL:         return byteInstruction("OP_DEC_LOCAL", chunk, offset);
    case OP_GET_PROPERTY:      return cachedInstruction("OP_GET_PROPERTY", chunk, offset);
    case OP_SET_PROPERTY_POP:  return cachedInstruction("OP_SET_PROPERTY_POP", chunk, offset);
    case OP_GET_LOCAL_LOCAL: {
      printf("%-22s %4d %d\n", "OP_GET_LOCAL_LOCAL", chunk->code[offset + 1],
             chunk->code[offset + 2]);
      return offset + 3;
    }
    case OP_GET_LOCAL_PROPERTY: {
      /* A slot, then the same constant-and-cache pair OP_GET_PROPERTY takes. */
      int constant = readConstantIndex(chunk, offset + 2);
      printf("%-22s %4d '", "OP_GET_LOCAL_PROPERTY", chunk->code[offset + 1]);
      csValuePrint(chunk->constants.values[constant]);
      printf("'  cache %d\n", readConstantIndex(chunk, offset + 4));
      return offset + 6;
    }
    case OP_SET_PROPERTY:      return cachedInstruction("OP_SET_PROPERTY", chunk, offset);
    case OP_GET_INDEX:         return simpleInstruction("OP_GET_INDEX", offset);
    case OP_ITER_LENGTH:       return simpleInstruction("OP_ITER_LENGTH", offset);
    case OP_ENUM_KEYS:         return simpleInstruction("OP_ENUM_KEYS", offset);
    case OP_SET_INDEX:         return simpleInstruction("OP_SET_INDEX", offset);
    case OP_OBJECT:            return byteInstruction("OP_OBJECT", chunk, offset);
    case OP_ARRAY:             return byteInstruction("OP_ARRAY", chunk, offset);
    case OP_SPREAD_MARK:       return simpleInstruction("OP_SPREAD_MARK", offset);
    case OP_ARRAY_SPREAD:      return byteInstruction("OP_ARRAY_SPREAD", chunk, offset);
    case OP_ARRAY_REST:        return byteInstruction("OP_ARRAY_REST", chunk, offset);
    case OP_CALL_SPREAD:       return simpleInstruction("OP_CALL_SPREAD", offset);
    case OP_CLOSURE: {
      /* Followed by one (isLocal, index) pair per upvalue, which are operands
       * rather than instructions. */
      int constant = readConstantIndex(chunk, offset + 1);
      printf("%-22s %4d '", "OP_CLOSURE", constant);
      csValuePrint(chunk->constants.values[constant]);
      printf("'\n");

      int next = offset + 3;
      Value function = chunk->constants.values[constant];
      if (IS_FUNCTION(function)) {
        for (int i = 0; i < AS_FUNCTION(function)->upvalueCount; i++) {
          printf("%04d      |                     %s %d\n", next,
                 chunk->code[next] ? "local" : "upvalue", chunk->code[next + 1]);
          next += 2;
        }
      }
      return next;
    }
    case OP_GET_UPVALUE:       return byteInstruction("OP_GET_UPVALUE", chunk, offset);
    case OP_SET_UPVALUE:       return byteInstruction("OP_SET_UPVALUE", chunk, offset);
    case OP_CLOSE_UPVALUE:     return simpleInstruction("OP_CLOSE_UPVALUE", offset);
    case OP_CALL:              return byteInstruction("OP_CALL", chunk, offset);
    case OP_CLASS:             return constantInstruction("OP_CLASS", chunk, offset);
    case OP_INHERIT:           return simpleInstruction("OP_INHERIT", offset);
    case OP_CONSTRUCTOR:       return simpleInstruction("OP_CONSTRUCTOR", offset);
    case OP_FIELD_INIT:        return simpleInstruction("OP_FIELD_INIT", offset);
    case OP_INSTANCEOF:        return simpleInstruction("OP_INSTANCEOF", offset);
    case OP_IMPORT_NAME:       return constantInstruction("OP_IMPORT_NAME", chunk, offset);
    case OP_IMPORT_NAMESPACE:  return simpleInstruction("OP_IMPORT_NAMESPACE", offset);
    case OP_AWAIT:             return simpleInstruction("OP_AWAIT", offset);
    case OP_NEW:               return byteInstruction("OP_NEW", chunk, offset);
    case OP_SUPER_CALL:        return byteInstruction("OP_SUPER_CALL", chunk, offset);
    case OP_METHOD:            return constantInstruction("OP_METHOD", chunk, offset);
    case OP_STATIC_METHOD:     return constantInstruction("OP_STATIC_METHOD", chunk, offset);
    case OP_STATIC_FIELD:      return constantInstruction("OP_STATIC_FIELD", chunk, offset);
    case OP_GETTER:            return constantInstruction("OP_GETTER", chunk, offset);
    case OP_SETTER:            return constantInstruction("OP_SETTER", chunk, offset);
    case OP_GET_SUPER:         return constantInstruction("OP_GET_SUPER", chunk, offset);
    case OP_SUPER_INVOKE:      return invokeInstruction("OP_SUPER_INVOKE", chunk, offset);
    case OP_INVOKE:            return invokeInstruction("OP_INVOKE", chunk, offset);
    case OP_ADD:               return simpleInstruction("OP_ADD", offset);
    case OP_ADD_NUM:           return simpleInstruction("OP_ADD_NUM", offset);
    case OP_SUBTRACT:          return simpleInstruction("OP_SUBTRACT", offset);
    case OP_MULTIPLY:          return simpleInstruction("OP_MULTIPLY", offset);
    case OP_DIVIDE:            return simpleInstruction("OP_DIVIDE", offset);
    case OP_MODULO:            return simpleInstruction("OP_MODULO", offset);
    case OP_EXPONENT:          return simpleInstruction("OP_EXPONENT", offset);
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
    case OP_TRY:               return jumpInstruction("OP_TRY", 1, chunk, offset);
    case OP_END_TRY:           return simpleInstruction("OP_END_TRY", offset);
    case OP_THROW:             return simpleInstruction("OP_THROW", offset);
    case OP_JUMP_IF_NOT_LESS:          return jumpInstruction("OP_JUMP_IF_NOT_LESS", 1, chunk, offset);
    case OP_JUMP_IF_NOT_LESS_EQUAL:    return jumpInstruction("OP_JUMP_IF_NOT_LESS_EQUAL", 1, chunk, offset);
    case OP_JUMP_IF_NOT_GREATER:       return jumpInstruction("OP_JUMP_IF_NOT_GREATER", 1, chunk, offset);
    case OP_JUMP_IF_NOT_GREATER_EQUAL: return jumpInstruction("OP_JUMP_IF_NOT_GREATER_EQUAL", 1, chunk, offset);
    case OP_JUMP_IF_NOT_EQUAL:         return jumpInstruction("OP_JUMP_IF_NOT_EQUAL", 1, chunk, offset);
    case OP_JUMP_IF_EQUAL:             return jumpInstruction("OP_JUMP_IF_EQUAL", 1, chunk, offset);
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

/* Annotates a dumped node with the type the checker resolved, so `make trace`
 * shows what the compiler actually knows. */
static void printType(const AstNode *node) {
  if (node->resolvedType != TYPE_ANY) printf("  : %s", csTypeName(node->resolvedType));
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
      printType(node);
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
      printf("Binary %s", csBinaryOpName(node->as.binary.op));
      printType(node);
      printf("\n");
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
      printf("Identifier %.*s", node->as.identifier.length, node->as.identifier.name);
      printType(node);
      printf("\n");
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
    case AST_FUNCTION:
      printf("Function %.*s (%d param%s)\n",
             node->as.function.name != NULL ? node->as.function.nameLength : 11,
             node->as.function.name != NULL ? node->as.function.name : "<anonymous>",
             node->as.function.paramCount,
             node->as.function.paramCount == 1 ? "" : "s");
      printNode(node->as.function.body, depth + 1);
      break;
    case AST_RETURN_STMT:
      printf("Return\n");
      printNode(node->as.returnValue, depth + 1);
      break;
    case AST_CONDITIONAL:
      printf("Conditional\n");
      printNode(node->as.conditional.condition, depth + 1);
      printNode(node->as.conditional.thenValue, depth + 1);
      printNode(node->as.conditional.elseValue, depth + 1);
      break;
    case AST_BREAK_STMT:
      printf("Break\n");
      break;
    case AST_CONTINUE_STMT:
      printf("Continue\n");
      break;
    case AST_SWITCH_STMT:
      printf("Switch (%d case%s)\n", node->as.switchStmt.caseCount,
             node->as.switchStmt.caseCount == 1 ? "" : "s");
      printNode(node->as.switchStmt.subject, depth + 1);
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        printNode(node->as.switchStmt.cases[i].test, depth + 1);
        printNode(node->as.switchStmt.cases[i].body, depth + 2);
      }
      if (node->as.switchStmt.defaultBody != NULL) {
        printNode(node->as.switchStmt.defaultBody, depth + 1);
      }
      break;
    case AST_TRY_STMT:
      printf("Try\n");
      printNode(node->as.tryStmt.body, depth + 1);
      if (node->as.tryStmt.catchBody != NULL) {
        printNode(node->as.tryStmt.catchBody, depth + 1);
      }
      if (node->as.tryStmt.finallyBody != NULL) {
        printNode(node->as.tryStmt.finallyBody, depth + 1);
      }
      break;
    case AST_THROW_STMT:
      printf("Throw\n");
      printNode(node->as.thrown, depth + 1);
      break;
    case AST_IMPORT:
      printf("Import \"%.*s\"\n", node->as.import.specifierLength,
             node->as.import.specifier);
      break;
    case AST_EXPORT:
      printf("Export\n");
      printNode(node->as.export.declaration, depth + 1);
      break;
    case AST_AWAIT:
      printf("Await\n");
      printNode(node->as.unary.operand, depth + 1);
      break;
    case AST_THIS:
      printf("This\n");
      break;
    case AST_SUPER:
      if (node->as.super.name != NULL) {
        printf("Super .%.*s\n", node->as.super.length, node->as.super.name);
      } else {
        printf("Super ()\n");
      }
      break;
    case AST_CLASS_DECL:
      printf("Class %.*s", node->as.classDecl.nameLength, node->as.classDecl.name);
      if (node->as.classDecl.superName != NULL) {
        printf(" extends %.*s", node->as.classDecl.superLength,
               node->as.classDecl.superName);
      }
      printf("\n");
      for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
        indent(depth + 1);
        printf("Field %.*s\n", node->as.classDecl.fields[i].length,
               node->as.classDecl.fields[i].name);
        printNode(node->as.classDecl.fields[i].initializer, depth + 2);
      }
      printNode(node->as.classDecl.constructor, depth + 1);
      for (int i = 0; i < node->as.classDecl.memberCount; i++) {
        printNode(node->as.classDecl.members[i].function, depth + 1);
      }
      break;
    case AST_SPREAD:
      printf("Spread\n");
      printNode(node->as.spread, depth + 1);
      break;
    case AST_DESTRUCTURE:
      printf("Destructure %s (%d binding%s)\n",
             node->as.destructure.isObject ? "object" : "array",
             node->as.destructure.count,
             node->as.destructure.count == 1 ? "" : "s");
      printNode(node->as.destructure.initializer, depth + 1);
      break;
    case AST_INDEX:
      printf("Index\n");
      printNode(node->as.index.target, depth + 1);
      printNode(node->as.index.index, depth + 1);
      break;
    case AST_OBJECT_LITERAL:
      printf("ObjectLiteral (%d)\n", node->as.objectLiteral.count);
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        printNode(node->as.objectLiteral.keys[i], depth + 1);
        printNode(node->as.objectLiteral.values[i], depth + 2);
      }
      break;
    case AST_ARRAY_LITERAL:
      printf("ArrayLiteral (%d)\n", node->as.arrayLiteral.count);
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        printNode(node->as.arrayLiteral.elements[i], depth + 1);
      }
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
    case AST_FOR_OF_STMT:
      printf("ForOf %.*s\n", node->as.forOf.nameLength, node->as.forOf.name);
      printNode(node->as.forOf.iterable, depth + 1);
      printNode(node->as.forOf.body, depth + 1);
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
