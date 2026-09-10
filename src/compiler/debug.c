/* debug.c — the disassembler, and the flags that ask for a dump.
 *
 * This doubles as the one place that knows how long each instruction is:
 * csInstructionLength walks the same operand layout without printing, and the
 * tiering pass and the lowering both stride a chunk with it. Anything else
 * needing that would otherwise keep its own copy, and a copy that drifts reads
 * an operand as an opcode.
 */
#include <stdarg.h>
#include <stdio.h>

#include "cscript/debug.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/value.h"

/* The trace build asks for every stage, which is what it always did. Anything
 * else starts with none and the command line adds them. */
unsigned csDumpStages =
#ifdef CS_DEBUG_PRINT_TOKENS
    CS_DUMP_TOKENS |
#endif
#ifdef CS_DEBUG_PRINT_AST
    CS_DUMP_AST |
#endif
#ifdef CS_DEBUG_PRINT_CODE
    CS_DUMP_BYTECODE |
#endif
    0u;

/* The disassembler doubles as the one place that knows how long each
 * instruction is. Anything else needing that — the tiering pass, which walks a
 * chunk looking for opcodes a backend could not emit — would otherwise have to
 * keep a second copy of the operand layout, and the two would drift.
 *
 * So it can run silently: same walk, no output, just the next offset. */
static bool quiet = false;

static void emit(const char *format, ...) {
  if (quiet) return;
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

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

/* csValuePrint writes straight to stdout, so it needs the same gate. */
static void debugPrintValue(Value value) {
  if (quiet) return;
  csValuePrint(value);
}

static int simpleInstruction(const char *name, int offset) {
  emit("%s\n", name);
  return offset + 1;
}

/* Constant indices are two bytes, big-endian. */
static int readConstantIndex(const Chunk *chunk, int offset) {
  return (chunk->code[offset] << 8) | chunk->code[offset + 1];
}

static int constantInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  emit("%-22s %4d '", name, constant);
  debugPrintValue(chunk->constants.values[constant]);
  emit("'\n");
  return offset + 3;
}

/* A constant index followed by an inline-cache index. The cache index is
 * printed because a site that is not hitting is usually easiest to find by
 * matching it back to the entry in the chunk's cache array. */
static int cachedInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  int cache = readConstantIndex(chunk, offset + 3);
  emit("%-22s %4d '", name, constant);
  debugPrintValue(chunk->constants.values[constant]);
  emit("'  cache %d\n", cache);
  return offset + 5;
}

/* Two operands: a method name and an argument count. */
static int invokeInstruction(const char *name, const Chunk *chunk, int offset) {
  int constant = readConstantIndex(chunk, offset + 1);
  uint8_t argCount = chunk->code[offset + 3];
  emit("%-22s %4d '", name, argCount);
  debugPrintValue(chunk->constants.values[constant]);
  emit("'\n");
  return offset + 4;
}

static int byteInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t operand = chunk->code[offset + 1];
  emit("%-22s %4d\n", name, operand);
  return offset + 2;
}

/* Two operands: a stack slot and a constant index. */
static int slotConstantInstruction(const char *name, const Chunk *chunk, int offset) {
  uint8_t slot = chunk->code[offset + 1];
  int constant = readConstantIndex(chunk, offset + 2);
  emit("%-22s %4d %4d '", name, slot, constant);
  debugPrintValue(chunk->constants.values[constant]);
  emit("'\n");
  return offset + 4;
}

static int jumpInstruction(const char *name, int sign, const Chunk *chunk, int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
  jump |= chunk->code[offset + 2];
  emit("%-22s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
}

/* The offset just past the instruction at `offset`, without printing it. */
int csInstructionLength(const Chunk *chunk, int offset) {
  quiet = true;
  int next = csDisassembleInstruction(chunk, offset);
  quiet = false;
  return next;
}

int csDisassembleInstruction(const Chunk *chunk, int offset) {
  emit("%04d ", offset);

  /* Repeat the line number only when it changes, so runs read as blocks. */
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    emit("   | ");
  } else {
    emit("%4d ", chunk->lines[offset]);
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
    case OP_DUP2:              return simpleInstruction("OP_DUP2", offset);
    case OP_POP_UNDER:         return simpleInstruction("OP_POP_UNDER", offset);
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
      emit("%-22s %4d %d\n", "OP_GET_LOCAL_LOCAL", chunk->code[offset + 1],
             chunk->code[offset + 2]);
      return offset + 3;
    }
    case OP_GET_LOCAL_PROPERTY: {
      /* A slot, then the same constant-and-cache pair OP_GET_PROPERTY takes. */
      int constant = readConstantIndex(chunk, offset + 2);
      emit("%-22s %4d '", "OP_GET_LOCAL_PROPERTY", chunk->code[offset + 1]);
      debugPrintValue(chunk->constants.values[constant]);
      emit("'  cache %d\n", readConstantIndex(chunk, offset + 4));
      return offset + 6;
    }
    case OP_SET_PROPERTY:      return cachedInstruction("OP_SET_PROPERTY", chunk, offset);
    case OP_GET_INDEX:         return simpleInstruction("OP_GET_INDEX", offset);
    case OP_DESTRUCTURE_PREPARE:
      return byteInstruction("OP_DESTRUCTURE_PREPARE", chunk, offset);
    case OP_ITER_STEP:         return jumpInstruction("OP_ITER_STEP", 1, chunk, offset);
    case OP_JUMP_IF_ASYNC_ITER:
      return jumpInstruction("OP_JUMP_IF_ASYNC_ITER", 1, chunk, offset);
    case OP_ASYNC_NEXT:        return simpleInstruction("OP_ASYNC_NEXT", offset);
    case OP_ITER_UNPACK:       return jumpInstruction("OP_ITER_UNPACK", 1, chunk, offset);
    case OP_ITER_LENGTH:       return simpleInstruction("OP_ITER_LENGTH", offset);
    case OP_ENUM_KEYS:         return simpleInstruction("OP_ENUM_KEYS", offset);
    case OP_ITER_PREPARE:      return byteInstruction("OP_ITER_PREPARE", chunk, offset);
    case OP_REGEX: {
      emit("%-22s /", "OP_REGEX");
      debugPrintValue(chunk->constants.values[readConstantIndex(chunk, offset + 1)]);
      emit("/");
      debugPrintValue(chunk->constants.values[readConstantIndex(chunk, offset + 3)]);
      emit("\n");
      return offset + 5;
    }
    case OP_SET_INDEX:         return simpleInstruction("OP_SET_INDEX", offset);
    case OP_OBJECT:            return byteInstruction("OP_OBJECT", chunk, offset);
    case OP_ARRAY:             return byteInstruction("OP_ARRAY", chunk, offset);
    case OP_SPREAD_MARK:       return simpleInstruction("OP_SPREAD_MARK", offset);
    case OP_ARRAY_SPREAD:      return byteInstruction("OP_ARRAY_SPREAD", chunk, offset);
    case OP_GET_PRIVATE:       return constantInstruction("OP_GET_PRIVATE", chunk, offset);
    case OP_SET_PRIVATE:       return constantInstruction("OP_SET_PRIVATE", chunk, offset);
    case OP_DELETE_PROPERTY:
      return constantInstruction("OP_DELETE_PROPERTY", chunk, offset);
    case OP_DELETE_INDEX:      return simpleInstruction("OP_DELETE_INDEX", offset);
    case OP_OBJECT_SET:        return simpleInstruction("OP_OBJECT_SET", offset);
    case OP_SET_PROTOTYPE:     return simpleInstruction("OP_SET_PROTOTYPE", offset);
    case OP_NEW_TARGET:        return simpleInstruction("OP_NEW_TARGET", offset);
    case OP_DYNAMIC_IMPORT:    return simpleInstruction("OP_DYNAMIC_IMPORT", offset);
    case OP_INVOKE_INDEX:      return byteInstruction("OP_INVOKE_INDEX", chunk, offset);
    case OP_OBJECT_ACCESSOR:   return byteInstruction("OP_OBJECT_ACCESSOR", chunk, offset);
    case OP_OBJECT_MERGE:      return simpleInstruction("OP_OBJECT_MERGE", offset);
    case OP_OBJECT_REST:       return byteInstruction("OP_OBJECT_REST", chunk, offset);
    case OP_TEMPLATE_STRINGS:  return simpleInstruction("OP_TEMPLATE_STRINGS", offset);
    case OP_ARRAY_REST:        return byteInstruction("OP_ARRAY_REST", chunk, offset);
    case OP_CALL_SPREAD:       return simpleInstruction("OP_CALL_SPREAD", offset);
    case OP_CLOSURE: {
      /* Followed by one (isLocal, index) pair per upvalue, which are operands
       * rather than instructions. */
      int constant = readConstantIndex(chunk, offset + 1);
      emit("%-22s %4d '", "OP_CLOSURE", constant);
      debugPrintValue(chunk->constants.values[constant]);
      emit("'\n");

      int next = offset + 3;
      Value function = chunk->constants.values[constant];
      if (IS_FUNCTION(function)) {
        for (int i = 0; i < AS_FUNCTION(function)->upvalueCount; i++) {
          emit("%04d      |                     %s %d\n", next,
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
    case OP_IN:                return simpleInstruction("OP_IN", offset);
    case OP_INSTANCEOF:        return simpleInstruction("OP_INSTANCEOF", offset);
    case OP_IMPORT_NAME:       return constantInstruction("OP_IMPORT_NAME", chunk, offset);
    case OP_IMPORT_NAMESPACE:  return simpleInstruction("OP_IMPORT_NAMESPACE", offset);
    case OP_YIELD:             return simpleInstruction("OP_YIELD", offset);
    case OP_AWAIT:             return simpleInstruction("OP_AWAIT", offset);
    case OP_NEW:               return byteInstruction("OP_NEW", chunk, offset);
    case OP_SUPER_CALL:        return byteInstruction("OP_SUPER_CALL", chunk, offset);
    case OP_METHOD:            return constantInstruction("OP_METHOD", chunk, offset);
    case OP_STATIC_METHOD:     return constantInstruction("OP_STATIC_METHOD", chunk, offset);
    case OP_STATIC_FIELD:      return constantInstruction("OP_STATIC_FIELD", chunk, offset);
    case OP_GETTER:            return constantInstruction("OP_GETTER", chunk, offset);
    case OP_CLASS_MEMBER:       return byteInstruction("OP_CLASS_MEMBER", chunk, offset);
    case OP_SETTER:            return constantInstruction("OP_SETTER", chunk, offset);
    case OP_STATIC_GETTER:     return constantInstruction("OP_STATIC_GETTER", chunk, offset);
    case OP_STATIC_SETTER:     return constantInstruction("OP_STATIC_SETTER", chunk, offset);
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
    case OP_JUMP_IF_NO_METHOD: {
      /* [const16][hi][lo] — a name and then an offset. */
      uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
      uint16_t jump = (uint16_t)((chunk->code[offset + 3] << 8) | chunk->code[offset + 4]);
      emit("%-24s %4d '", "OP_JUMP_IF_NO_METHOD", constant);
      if (!quiet) csValuePrint(chunk->constants.values[constant]);
      emit("' -> %d\n", offset + 5 + jump);
      return offset + 5;
    }
    case OP_JUMP_IF_NULLISH:
      return jumpInstruction("OP_JUMP_IF_NULLISH", 1, chunk, offset);
    case OP_JUMP_IF_NOT_NULLISH:
      return jumpInstruction("OP_JUMP_IF_NOT_NULLISH", 1, chunk, offset);
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
      emit("unknown opcode %d\n", instruction);
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
