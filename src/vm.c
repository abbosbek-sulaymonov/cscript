#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/debug.h"
#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/parser.h"
#include "cscript/vm.h"

VM vm;

static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.tempRootCount = 0;
}

void csVMInit(void) {
  resetStack();
  vm.objects = NULL;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;
  vm.chunk = NULL;
  vm.ip = NULL;
  vm.sourceName = "<script>";

  csTableInit(&vm.globals);
  csTableInit(&vm.globalConsts);
  csTableInit(&vm.strings);

  csNativesInstall();
}

void csVMMarkGlobalConst(ObjString *name) {
  csTableSet(&vm.globalConsts, name, BOOL_VAL(true));
}

void csVMFree(void) {
  csTableFree(&vm.globals);
  csTableFree(&vm.globalConsts);
  csTableFree(&vm.strings);
  csFreeAllObjects();
}

void csVMPush(Value value) {
  if (vm.stackTop - vm.stack >= CS_STACK_MAX) {
    fprintf(stderr, "cscript: value stack overflow\n");
    resetStack();
    return;
  }
  *vm.stackTop++ = value;
}

Value csVMPop(void) { return *(--vm.stackTop); }

static Value peekStack(int distance) { return vm.stackTop[-1 - distance]; }

void csVMRuntimeError(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "cscript: runtime error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  if (vm.chunk != NULL && vm.ip != NULL) {
    /* `ip` has already advanced past the failing instruction. */
    size_t instruction = (size_t)(vm.ip - vm.chunk->code - 1);
    int line = vm.chunk->lines[instruction];
    fprintf(stderr, "  at %s:%d\n", vm.sourceName, line);
  }

  resetStack();
}

/* JS `+`: if either side is a string, both are stringified and concatenated;
 * otherwise both are coerced to numbers and added. */
static bool concatenateOrAdd(void) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (IS_STRING(a) || IS_STRING(b)) {
    size_t aLength = 0;
    size_t bLength = 0;
    char *aText = csValueToCString(a, &aLength);
    char *bText = csValueToCString(b, &bLength);
    if (aText == NULL || bText == NULL) {
      free(aText);
      free(bText);
      csVMRuntimeError("out of memory while concatenating strings");
      return false;
    }

    size_t length = aLength + bLength;
    char *joined = CS_ALLOCATE(char, length + 1);
    memcpy(joined, aText, aLength);
    memcpy(joined + aLength, bText, bLength);
    joined[length] = '\0';
    free(aText);
    free(bText);

    ObjString *result = csStringTakeOwnership(joined, (int)length);
    csVMPop();
    csVMPop();
    csVMPush(OBJ_VAL(result));
    return true;
  }

  if (!IS_NUMBER(a) || !IS_NUMBER(b)) {
    csVMRuntimeError("cannot add %s and %s", csValueTypeName(a), csValueTypeName(b));
    return false;
  }

  csVMPop();
  csVMPop();
  csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
  return true;
}

/* Calls a native function sitting `argCount` slots below the stack top. */
static bool callValue(Value callee, int argCount) {
  if (!IS_NATIVE(callee)) {
    csVMRuntimeError("%s is not a function", csValueTypeName(callee));
    return false;
  }

  ObjNative *native = AS_NATIVE(callee);
  if (native->arity >= 0 && argCount != native->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d", native->name->chars,
                     native->arity, native->arity == 1 ? "" : "s", argCount);
    return false;
  }

  Value result;
  if (!native->function(argCount, vm.stackTop - argCount, &result)) return false;

  /* Drop the arguments and the callee, then push the result in its place. */
  vm.stackTop -= argCount + 1;
  csVMPush(result);
  return true;
}

static InterpretResult run(void) {
#define READ_BYTE() (*vm.ip++)
#define READ_SHORT() (vm.ip += 2, (uint16_t)((vm.ip[-2] << 8) | vm.ip[-1]))
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

/* Arithmetic and comparison share this shape: both operands must be numbers.
 * JavaScript would coerce and often produce NaN; an error at the mistake is
 * far easier to debug than a NaN that spreads silently. */
#define BINARY_NUMERIC_OP(valueType, op)                                    \
  do {                                                                      \
    if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {             \
      csVMRuntimeError("operands of '" #op "' must be numbers, got %s and %s", \
                       csValueTypeName(peekStack(1)),                       \
                       csValueTypeName(peekStack(0)));                      \
      return CS_RUNTIME_ERROR;                                              \
    }                                                                       \
    double b = AS_NUMBER(csVMPop());                                        \
    double a = AS_NUMBER(csVMPop());                                        \
    csVMPush(valueType(a op b));                                            \
  } while (false)

  for (;;) {
#ifdef CS_DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      csValuePrint(*slot);
      printf(" ]");
    }
    printf("\n");
    csDisassembleInstruction(vm.chunk, (int)(vm.ip - vm.chunk->code));
#endif

    uint8_t instruction = READ_BYTE();
    switch (instruction) {
      case OP_CONSTANT:  csVMPush(READ_CONSTANT()); break;
      case OP_NULL:      csVMPush(NULL_VAL); break;
      case OP_UNDEFINED: csVMPush(UNDEFINED_VAL); break;
      case OP_TRUE:      csVMPush(BOOL_VAL(true)); break;
      case OP_FALSE:     csVMPush(BOOL_VAL(false)); break;

      case OP_POP: csVMPop(); break;
      case OP_POP_N: vm.stackTop -= READ_BYTE(); break;
      case OP_DUP: csVMPush(peekStack(0)); break;

      case OP_DEFINE_GLOBAL: {
        ObjString *name = READ_STRING();
        csTableSet(&vm.globals, name, peekStack(0));
        csVMPop();
        break;
      }

      case OP_DEFINE_CONST: {
        ObjString *name = READ_STRING();
        csTableSet(&vm.globals, name, peekStack(0));
        csVMMarkGlobalConst(name);
        csVMPop();
        break;
      }

      case OP_GET_GLOBAL: {
        ObjString *name = READ_STRING();
        Value value;
        if (!csTableGet(&vm.globals, name, &value)) {
          /* JavaScript would return undefined for a bare read in sloppy mode.
           * Reading a name that was never declared is a typo, not an intent. */
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPush(value);
        break;
      }

      case OP_SET_GLOBAL: {
        ObjString *name = READ_STRING();
        if (csTableGet(&vm.globalConsts, name, NULL)) {
          csVMRuntimeError("'%s' is a constant and cannot be reassigned", name->chars);
          return CS_RUNTIME_ERROR;
        }
        /* csTableSet reports whether the key was new; assigning to a name that
         * does not exist would silently create a global in JavaScript. */
        if (csTableSet(&vm.globals, name, peekStack(0))) {
          csTableDelete(&vm.globals, name);
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        break;
      }

      case OP_GET_LOCAL: csVMPush(vm.stack[READ_BYTE()]); break;

      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        vm.stack[slot] = peekStack(0);
        break;
      }

      case OP_GET_PROPERTY: {
        ObjString *name = READ_STRING();
        Value receiver = peekStack(0);
        if (!IS_OBJECT(receiver)) {
          csVMRuntimeError("cannot read property '%s' of %s", name->chars,
                           csValueTypeName(receiver));
          return CS_RUNTIME_ERROR;
        }
        Value value;
        if (!csTableGet(&AS_OBJECT(receiver)->properties, name, &value)) {
          csVMRuntimeError("'%s' has no property '%s'",
                           AS_OBJECT(receiver)->name->chars, name->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        csVMPush(value);
        break;
      }

      case OP_CALL: {
        int argCount = READ_BYTE();
        if (!callValue(peekStack(argCount), argCount)) return CS_RUNTIME_ERROR;
        break;
      }

      case OP_ADD:
        if (!concatenateOrAdd()) return CS_RUNTIME_ERROR;
        break;

      case OP_SUBTRACT: BINARY_NUMERIC_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY: BINARY_NUMERIC_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE:   BINARY_NUMERIC_OP(NUMBER_VAL, /); break;

      case OP_MODULO: {
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '%%' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double b = AS_NUMBER(csVMPop());
        double a = AS_NUMBER(csVMPop());
        /* JS % keeps the sign of the dividend, which is exactly fmod. */
        csVMPush(NUMBER_VAL(fmod(a, b)));
        break;
      }

      case OP_NEGATE:
        if (!IS_NUMBER(peekStack(0))) {
          csVMRuntimeError("operand of unary '-' must be a number, got %s",
                           csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(NUMBER_VAL(-AS_NUMBER(csVMPop())));
        break;

      case OP_NOT:
        csVMPush(BOOL_VAL(!csValueIsTruthy(csVMPop())));
        break;

      case OP_TYPEOF: {
        const char *name = csValueTypeName(csVMPop());
        csVMPush(OBJ_VAL(csStringCopy(name, (int)strlen(name))));
        break;
      }

      case OP_EQUAL: {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(csValuesStrictEqual(a, b)));
        break;
      }
      case OP_NOT_EQUAL: {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(!csValuesStrictEqual(a, b)));
        break;
      }

      case OP_GREATER:       BINARY_NUMERIC_OP(BOOL_VAL, >); break;
      case OP_GREATER_EQUAL: BINARY_NUMERIC_OP(BOOL_VAL, >=); break;
      case OP_LESS:          BINARY_NUMERIC_OP(BOOL_VAL, <); break;
      case OP_LESS_EQUAL:    BINARY_NUMERIC_OP(BOOL_VAL, <=); break;

      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        vm.ip += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(peekStack(0))) vm.ip += offset;
        break;
      }
      case OP_JUMP_IF_TRUE: {
        uint16_t offset = READ_SHORT();
        if (csValueIsTruthy(peekStack(0))) vm.ip += offset;
        break;
      }
      case OP_POP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(csVMPop())) vm.ip += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        vm.ip -= offset;
        break;
      }

      case OP_RETURN:
        return CS_OK;

      default:
        csVMRuntimeError("unknown opcode %d", instruction);
        return CS_RUNTIME_ERROR;
    }
  }

#undef BINARY_NUMERIC_OP
#undef READ_STRING
#undef READ_CONSTANT
#undef READ_SHORT
#undef READ_BYTE
}

InterpretResult csInterpret(const char *source, const char *sourceName) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, sourceName);
  vm.sourceName = sourceName;

#ifdef CS_DEBUG_PRINT_TOKENS
  csLexerDumpTokens(source, &diag);
  csDiagnosticsInit(&diag, source, sourceName); /* reset after the dry run */
#endif

  AstArena arena;
  csAstArenaInit(&arena);

  AstNode *program = csParse(source, &arena, &diag);
  if (program == NULL) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

#ifdef CS_DEBUG_PRINT_AST
  csAstPrint(program);
#endif

  Chunk chunk;
  csChunkInit(&chunk);

  bool compiled = csCompile(program, &chunk, &diag);
  /* The AST is only needed to produce bytecode, so it goes as soon as it has. */
  csAstArenaFree(&arena);

  if (!compiled) {
    csChunkFree(&chunk);
    return CS_COMPILE_ERROR;
  }

#ifdef CS_DEBUG_PRINT_CODE
  csDisassembleChunk(&chunk, sourceName);
#endif

  vm.chunk = &chunk;
  vm.ip = chunk.code;
  InterpretResult result = run();

  vm.chunk = NULL;
  vm.ip = NULL;
  csChunkFree(&chunk);
  return result;
}
