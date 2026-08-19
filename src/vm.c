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
#include "cscript/typecheck.h"
#include "cscript/vm.h"

VM vm;

static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
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

/* True when a double holds an exact integer small enough to move into an
 * int64_t without loss. 2^53 is the largest such value for a double, and it is
 * far inside int64_t's range, so the conversion below cannot overflow. */
static inline bool isExactInteger(double value) {
  return value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
         value == (double)(int64_t)value;
}

void csVMRuntimeError(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "cscript: runtime error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  /* Walk the frames innermost-first so the trace reads like a call stack. */
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    /* `ip` has already advanced past the failing instruction. */
    size_t instruction = (size_t)(frame->ip - function->chunk.code - 1);
    int line = function->chunk.lines[instruction];

    if (function->name == NULL) {
      fprintf(stderr, "  at %s:%d\n", vm.sourceName, line);
    } else {
      fprintf(stderr, "  at %s (%s:%d)\n", function->name->chars, vm.sourceName, line);
    }
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

/* Pushes a frame for a user function. The frame's window starts at the callee
 * itself, so slot 0 is the function and slots 1..arity are the arguments —
 * exactly where the compiler assigned the parameters. */
static bool callClosure(ObjClosure *closure, int argCount) {
  ObjFunction *function = closure->function;

  if (argCount != function->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d",
                     function->name != NULL ? function->name->chars : "<anonymous>",
                     function->arity, function->arity == 1 ? "" : "s", argCount);
    return false;
  }

  if (vm.frameCount == CS_FRAMES_MAX) {
    csVMRuntimeError("call stack overflow (limit %d frames)", CS_FRAMES_MAX);
    return false;
  }

  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callNative(ObjNative *native, int argCount) {
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

static bool callValue(Value callee, int argCount) {
  if (IS_CLOSURE(callee)) return callClosure(AS_CLOSURE(callee), argCount);
  if (IS_NATIVE(callee)) return callNative(AS_NATIVE(callee), argCount);

  csVMRuntimeError("%s is not a function", csValueTypeName(callee));
  return false;
}

/* Finds or creates the upvalue for a stack slot.
 *
 * The open list is kept sorted by descending slot address, so two closures
 * capturing the same variable share one upvalue — which is what makes writes
 * through one visible to the other. */
static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *previous = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;

  while (upvalue != NULL && upvalue->location > local) {
    previous = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) return upvalue;

  ObjUpvalue *created = csUpvalueNew(local);
  created->next = upvalue;
  if (previous == NULL) {
    vm.openUpvalues = created;
  } else {
    previous->next = created;
  }
  return created;
}

/* Moves every upvalue at or above `last` off the stack and onto its own heap
 * cell, so closures keep working once the frame is gone. */
static void closeUpvalues(Value *last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

#ifdef CS_DEBUG_PROFILE_OPCODES
/* Counts how often each opcode follows each other opcode, so superinstruction
 * candidates are chosen from data rather than from intuition. */
static uint64_t opcodePairs[OP_COUNT][OP_COUNT];
static uint64_t opcodeCounts[OP_COUNT];
static int previousOpcode = -1;

static void recordOpcode(uint8_t opcode) {
  opcodeCounts[opcode]++;
  if (previousOpcode >= 0) opcodePairs[previousOpcode][opcode]++;
  previousOpcode = opcode;
}

void csVMDumpOpcodeProfile(void);

void csVMDumpOpcodeProfile(void) {
  uint64_t total = 0;
  for (int i = 0; i < OP_COUNT; i++) total += opcodeCounts[i];
  if (total == 0) return;

  fprintf(stderr, "\n== opcode profile: %llu instructions ==\n",
          (unsigned long long)total);

  /* Top single opcodes. */
  fprintf(stderr, "\n-- most executed --\n");
  for (int rank = 0; rank < 10; rank++) {
    int best = -1;
    for (int i = 0; i < OP_COUNT; i++) {
      if (opcodeCounts[i] > 0 && (best < 0 || opcodeCounts[i] > opcodeCounts[best])) {
        best = i;
      }
    }
    if (best < 0) break;
    fprintf(stderr, "  %-22s %10llu  %5.1f%%\n", csOpcodeName((OpCode)best),
            (unsigned long long)opcodeCounts[best],
            100.0 * (double)opcodeCounts[best] / (double)total);
    opcodeCounts[best] = 0;
  }

  /* Top adjacent pairs — each one is a superinstruction candidate. */
  fprintf(stderr, "\n-- most frequent pairs --\n");
  for (int rank = 0; rank < 15; rank++) {
    int bestA = -1, bestB = -1;
    uint64_t best = 0;
    for (int a = 0; a < OP_COUNT; a++) {
      for (int b = 0; b < OP_COUNT; b++) {
        if (opcodePairs[a][b] > best) {
          best = opcodePairs[a][b];
          bestA = a;
          bestB = b;
        }
      }
    }
    if (bestA < 0) break;
    fprintf(stderr, "  %-22s -> %-22s %9llu  %5.1f%%\n", csOpcodeName((OpCode)bestA),
            csOpcodeName((OpCode)bestB), (unsigned long long)best,
            100.0 * (double)best / (double)total);
    opcodePairs[bestA][bestB] = 0;
  }
  fprintf(stderr, "\n");
}
#endif

/* Dispatch strategy.
 *
 * A switch compiles to one indirect branch shared by every opcode, so the CPU's
 * branch predictor has a single history slot for the whole interpreter and
 * mispredicts constantly. Computed goto gives each opcode its own dispatch
 * branch at the end of its own handler, so the predictor can learn the pairs
 * that actually follow one another — OP_GET_LOCAL is nearly always followed by
 * OP_CONSTANT in a loop, and that becomes predictable.
 *
 * The switch version is kept for compilers without the labels-as-values
 * extension, and both paths run the same handler bodies.
 */
/* Define CS_NO_COMPUTED_GOTO to force the portable switch path — `make
 * test-switch` builds that way, so the fallback cannot quietly rot. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(CS_NO_COMPUTED_GOTO)
#define CS_COMPUTED_GOTO 1
#endif

/* Labels-as-values is a GNU extension, which -Wpedantic flags. The use is
 * deliberate and guarded by the feature test above, so the warning is silenced
 * here rather than dropped from the project-wide warning set. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

static InterpretResult run(void) {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

/* `frame` is cached in a local rather than re-read from vm.frames each time;
 * it is refreshed on every call and return. */
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() \
  (frame->closure->function->chunk.constants.values[READ_SHORT()])
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

#ifdef CS_DEBUG_TRACE_EXECUTION
#define VM_TRACE_STEP()                                        \
  do {                                                         \
    printf("          ");                                      \
    for (Value *slot = vm.stack; slot < vm.stackTop; slot++) { \
      printf("[ ");                                            \
      csValuePrint(*slot);                                     \
      printf(" ]");                                            \
    }                                                          \
    printf("\n");                                              \
    csDisassembleInstruction(&frame->closure->function->chunk,                \
                             (int)(frame->ip - frame->closure->function->chunk.code)); \
  } while (false)
#else
#define VM_TRACE_STEP() ((void)0)
#endif

  uint8_t instruction = 0;

/* Defined outside the dispatch fork so both the computed-goto and the switch
 * path use the same profiling hook. */
#ifdef CS_DEBUG_PROFILE_OPCODES
#define VM_PROFILE_STEP() recordOpcode(instruction)
#else
#define VM_PROFILE_STEP() ((void)0)
#endif

#ifdef CS_COMPUTED_GOTO
  /* Generated from the same list as the OpCode enum, so the table can never
   * drift out of order with it. */
  static const void *const dispatchTable[] = {
#define CS_DISPATCH_ENTRY(name) &&label_##name,
      CS_OPCODE_LIST(CS_DISPATCH_ENTRY)
#undef CS_DISPATCH_ENTRY
  };
  _Static_assert(sizeof(dispatchTable) / sizeof(dispatchTable[0]) == OP_COUNT,
                 "dispatch table and OpCode enum disagree");

#define VM_DISPATCH()                            \
  do {                                           \
    VM_TRACE_STEP();                             \
    instruction = READ_BYTE();                   \
    VM_PROFILE_STEP();                           \
    goto *dispatchTable[instruction];            \
  } while (false)

#define VM_BEGIN VM_DISPATCH();
#define VM_CASE(name) label_##name:
#define VM_NEXT() VM_DISPATCH()
#define VM_END
#else
#define VM_BEGIN         \
  for (;;) {             \
    VM_TRACE_STEP();     \
    instruction = READ_BYTE(); \
    VM_PROFILE_STEP();   \
    switch (instruction) {
#define VM_CASE(name) case name:
#define VM_NEXT() break
#define VM_END \
  }            \
  }
#endif

  VM_BEGIN
      VM_CASE(OP_CONSTANT)  csVMPush(READ_CONSTANT()); VM_NEXT();
      VM_CASE(OP_NULL)      csVMPush(NULL_VAL); VM_NEXT();
      VM_CASE(OP_UNDEFINED) csVMPush(UNDEFINED_VAL); VM_NEXT();
      VM_CASE(OP_TRUE)      csVMPush(BOOL_VAL(true)); VM_NEXT();
      VM_CASE(OP_FALSE)     csVMPush(BOOL_VAL(false)); VM_NEXT();

      VM_CASE(OP_POP) csVMPop(); VM_NEXT();
      VM_CASE(OP_POP_N) vm.stackTop -= READ_BYTE(); VM_NEXT();
      VM_CASE(OP_DUP) csVMPush(peekStack(0)); VM_NEXT();

      VM_CASE(OP_DEFINE_GLOBAL) {
        ObjString *name = READ_STRING();
        csTableSet(&vm.globals, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_DEFINE_CONST) {
        ObjString *name = READ_STRING();
        csTableSet(&vm.globals, name, peekStack(0));
        csVMMarkGlobalConst(name);
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_GET_GLOBAL) {
        ObjString *name = READ_STRING();
        Value value;
        if (!csTableGet(&vm.globals, name, &value)) {
          /* JavaScript would return undefined for a bare read in sloppy mode.
           * Reading a name that was never declared is a typo, not an intent. */
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_GLOBAL) {
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
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL) csVMPush(frame->slots[READ_BYTE()]); VM_NEXT();

      VM_CASE(OP_SET_LOCAL) {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peekStack(0);
        VM_NEXT();
      }

      VM_CASE(OP_GET_LOCAL_CONST) {
        uint8_t slot = READ_BYTE();
        csVMPush(frame->slots[slot]);
        csVMPush(READ_CONSTANT());
        VM_NEXT();
      }

      VM_CASE(OP_SET_LOCAL_POP) {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_SET_GLOBAL_POP) {
        ObjString *name = READ_STRING();
        if (csTableGet(&vm.globalConsts, name, NULL)) {
          csVMRuntimeError("'%s' is a constant and cannot be reassigned", name->chars);
          return CS_RUNTIME_ERROR;
        }
        if (csTableSet(&vm.globals, name, peekStack(0))) {
          csTableDelete(&vm.globals, name);
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_INC_LOCAL)
      VM_CASE(OP_DEC_LOCAL) {
        uint8_t slot = READ_BYTE();
        Value *target = &frame->slots[slot];
        if (!IS_NUMBER(*target)) {
          csVMRuntimeError("operand of '%s' must be a number, got %s",
                           instruction == OP_INC_LOCAL ? "++" : "--",
                           csValueTypeName(*target));
          return CS_RUNTIME_ERROR;
        }
        /* Goes through the macros rather than mutating the payload in place,
         * so this works under either Value representation. */
        *target = NUMBER_VAL(AS_NUMBER(*target) +
                             (instruction == OP_INC_LOCAL ? 1 : -1));
        VM_NEXT();
      }

      VM_CASE(OP_GET_PROPERTY) {
        ObjString *name = READ_STRING();
        Value receiver = peekStack(0);

        /* `length` is intrinsic rather than a stored property, so arrays and
         * strings answer it without carrying a table. */
        if (name->length == 6 && memcmp(name->chars, "length", 6) == 0) {
          if (IS_ARRAY(receiver)) {
            csVMPop();
            csVMPush(NUMBER_VAL(AS_ARRAY(receiver)->elements.count));
            VM_NEXT();
          }
          if (IS_STRING(receiver)) {
            csVMPop();
            csVMPush(NUMBER_VAL(AS_STRING(receiver)->length));
            VM_NEXT();
          }
        }

        if (!IS_OBJECT(receiver)) {
          csVMRuntimeError("cannot read property '%s' of %s", name->chars,
                           csValueTypeName(receiver));
          return CS_RUNTIME_ERROR;
        }

        Value value;
        if (!csTableGet(&AS_OBJECT(receiver)->properties, name, &value)) {
          /* Reading a missing property gives undefined, as in JavaScript —
           * checking for absence is too common to make it an error. */
          csVMPop();
          csVMPush(UNDEFINED_VAL);
          VM_NEXT();
        }
        csVMPop();
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_PROPERTY) {
        ObjString *name = READ_STRING();
        Value value = peekStack(0);
        Value receiver = peekStack(1);

        if (!IS_OBJECT(receiver)) {
          csVMRuntimeError("cannot set property '%s' of %s", name->chars,
                           csValueTypeName(receiver));
          return CS_RUNTIME_ERROR;
        }
        if (csTableGet(&vm.globalConsts, name, NULL) &&
            AS_OBJECT(receiver)->properties.count > 0 &&
            csTableGet(&AS_OBJECT(receiver)->properties, name, NULL)) {
          /* Namespace members such as console.log stay put. */
          csVMRuntimeError("'%s' is a built-in and cannot be replaced", name->chars);
          return CS_RUNTIME_ERROR;
        }

        csObjectPut(AS_OBJECT(receiver), name, value);
        /* Leave the assigned value: assignment is an expression. */
        vm.stackTop -= 2;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_GET_INDEX) {
        Value index = peekStack(0);
        Value target = peekStack(1);

        if (IS_ARRAY(target)) {
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("array index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjArray *array = AS_ARRAY(target);
          int slot = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          /* Out of range reads as undefined rather than trapping, matching the
           * property rule above. */
          csVMPush(slot >= 0 && slot < array->elements.count
                       ? array->elements.values[slot]
                       : UNDEFINED_VAL);
          VM_NEXT();
        }

        if (IS_OBJECT(target)) {
          if (!IS_STRING(index)) {
            csVMRuntimeError("object key must be a string, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          Value value;
          bool found = csTableGet(&AS_OBJECT(target)->properties, AS_STRING(index), &value);
          vm.stackTop -= 2;
          csVMPush(found ? value : UNDEFINED_VAL);
          VM_NEXT();
        }

        csVMRuntimeError("cannot index %s", csValueTypeName(target));
        return CS_RUNTIME_ERROR;
      }

      VM_CASE(OP_SET_INDEX) {
        Value value = peekStack(0);
        Value index = peekStack(1);
        Value target = peekStack(2);

        if (IS_ARRAY(target)) {
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("array index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjArray *array = AS_ARRAY(target);
          int slot = (int)AS_NUMBER(index);
          if (slot < 0) {
            csVMRuntimeError("array index %d is out of range", slot);
            return CS_RUNTIME_ERROR;
          }
          /* Writing past the end extends the array with undefined rather than
           * creating a hole, so it stays dense. */
          while (array->elements.count <= slot) {
            csValueArrayWrite(&array->elements, UNDEFINED_VAL);
          }
          array->elements.values[slot] = value;
        } else if (IS_OBJECT(target)) {
          if (!IS_STRING(index)) {
            csVMRuntimeError("object key must be a string, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          csObjectPut(AS_OBJECT(target), AS_STRING(index), value);
        } else {
          csVMRuntimeError("cannot index %s", csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 3;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT) {
        int count = READ_BYTE();
        ObjObject *object = csObjectNew("Object");
        /* The literal's keys and values are still on the stack, so they are
         * rooted; the new object is not until it is pushed. */
        csPushTempRoot((Obj *)object);

        Value *entries = vm.stackTop - (count * 2);
        for (int i = 0; i < count; i++) {
          csObjectPut(object, AS_STRING(entries[i * 2]), entries[i * 2 + 1]);
        }

        csPopTempRoot();
        vm.stackTop = entries;
        csVMPush(OBJ_VAL(object));
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY) {
        int count = READ_BYTE();
        ObjArray *array = csArrayNew();
        csPushTempRoot((Obj *)array);

        Value *elements = vm.stackTop - count;
        for (int i = 0; i < count; i++) {
          csValueArrayWrite(&array->elements, elements[i]);
        }

        csPopTempRoot();
        vm.stackTop = elements;
        csVMPush(OBJ_VAL(array));
        VM_NEXT();
      }

      VM_CASE(OP_CALL) {
        int argCount = READ_BYTE();
        if (!callValue(peekStack(argCount), argCount)) return CS_RUNTIME_ERROR;
        /* A user call pushed a frame, so the cached pointer is stale. */
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_CLOSURE) {
        ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure *closure = csClosureNew(function);
        csVMPush(OBJ_VAL(closure));

        /* The compiler emitted one (isLocal, index) pair per upvalue. A local
         * is captured from this frame; anything else is already an upvalue of
         * the enclosing closure and is simply shared. */
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          closure->upvalues[i] = isLocal ? captureUpvalue(frame->slots + index)
                                         : frame->closure->upvalues[index];
        }
        VM_NEXT();
      }

      VM_CASE(OP_GET_UPVALUE) {
        uint8_t slot = READ_BYTE();
        csVMPush(*frame->closure->upvalues[slot]->location);
        VM_NEXT();
      }

      VM_CASE(OP_SET_UPVALUE) {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peekStack(0);
        VM_NEXT();
      }

      VM_CASE(OP_CLOSE_UPVALUE)
        closeUpvalues(vm.stackTop - 1);
        csVMPop();
        VM_NEXT();

      VM_CASE(OP_ADD_NUM) {
        /* The checker guaranteed both operands are numbers, so there is
         * nothing to test. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        vm.stackTop -= 2;
        csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
        VM_NEXT();
      }

      VM_CASE(OP_ADD) {
        /* Adding two numbers is overwhelmingly the common case, and routing it
         * through concatenateOrAdd() costs a call plus two string checks. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        if (IS_NUMBER(a) && IS_NUMBER(b)) {
          vm.stackTop -= 2;
          csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
          VM_NEXT();
        }
        if (!concatenateOrAdd()) return CS_RUNTIME_ERROR;
        VM_NEXT();
      }

      VM_CASE(OP_SUBTRACT) BINARY_NUMERIC_OP(NUMBER_VAL, -); VM_NEXT();
      VM_CASE(OP_MULTIPLY) BINARY_NUMERIC_OP(NUMBER_VAL, *); VM_NEXT();
      VM_CASE(OP_DIVIDE)   BINARY_NUMERIC_OP(NUMBER_VAL, /); VM_NEXT();

      VM_CASE(OP_MODULO) {
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '%%' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double b = AS_NUMBER(csVMPop());
        double a = AS_NUMBER(csVMPop());

        /* fmod() is a libm call and costs roughly 20ns; integer remainder is
         * about 1ns. Loop counters are integers virtually always, so take the
         * integer path whenever it is exactly equivalent.
         *
         * C's % truncates toward zero and so keeps the sign of the dividend,
         * which is what JavaScript's % does too. A zero dividend is excluded so
         * that -0 %% n stays -0 rather than becoming +0. */
        if (b != 0 && a != 0 && isExactInteger(a) && isExactInteger(b)) {
          csVMPush(NUMBER_VAL((double)((int64_t)a % (int64_t)b)));
        } else {
          csVMPush(NUMBER_VAL(fmod(a, b)));
        }
        VM_NEXT();
      }

      VM_CASE(OP_NEGATE)
        if (!IS_NUMBER(peekStack(0))) {
          csVMRuntimeError("operand of unary '-' must be a number, got %s",
                           csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(NUMBER_VAL(-AS_NUMBER(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_NOT)
        csVMPush(BOOL_VAL(!csValueIsTruthy(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_TYPEOF) {
        const char *name = csValueTypeName(csVMPop());
        csVMPush(OBJ_VAL(csStringCopy(name, (int)strlen(name))));
        VM_NEXT();
      }

      VM_CASE(OP_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(csValuesStrictEqual(a, b)));
        VM_NEXT();
      }
      VM_CASE(OP_NOT_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(!csValuesStrictEqual(a, b)));
        VM_NEXT();
      }

      VM_CASE(OP_GREATER)       BINARY_NUMERIC_OP(BOOL_VAL, >); VM_NEXT();
      VM_CASE(OP_GREATER_EQUAL) BINARY_NUMERIC_OP(BOOL_VAL, >=); VM_NEXT();
      VM_CASE(OP_LESS)          BINARY_NUMERIC_OP(BOOL_VAL, <); VM_NEXT();
      VM_CASE(OP_LESS_EQUAL)    BINARY_NUMERIC_OP(BOOL_VAL, <=); VM_NEXT();

      VM_CASE(OP_JUMP) {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_TRUE) {
        uint16_t offset = READ_SHORT();
        if (csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_POP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(csVMPop())) frame->ip += offset;
        VM_NEXT();
      }
/* Fused compare-and-branch. The operand check is the same one the standalone
 * comparison performs, so the error message a program sees does not change. */
#define COMPARE_JUMP(op)                                                       \
  do {                                                                         \
    uint16_t offset = READ_SHORT();                                            \
    Value b = peekStack(0);                                                    \
    Value a = peekStack(1);                                                    \
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) {                                      \
      csVMRuntimeError("operands of '" #op "' must be numbers, got %s and %s", \
                       csValueTypeName(a), csValueTypeName(b));                 \
      return CS_RUNTIME_ERROR;                                                 \
    }                                                                          \
    vm.stackTop -= 2;                                                          \
    if (!(AS_NUMBER(a) op AS_NUMBER(b))) frame->ip += offset;                   \
  } while (false)

/* VM_NEXT stays outside the macro. Under computed goto it is a goto, but under
 * switch dispatch it is a `break` — which a do/while(false) would swallow,
 * falling through to the next case instead of dispatching. */
      VM_CASE(OP_JUMP_IF_NOT_LESS) COMPARE_JUMP(<); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_LESS_EQUAL) COMPARE_JUMP(<=); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER) COMPARE_JUMP(>); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER_EQUAL) COMPARE_JUMP(>=); VM_NEXT();

      /* Equality accepts any types, so it needs no operand check. */
      VM_CASE(OP_JUMP_IF_NOT_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (!csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

      VM_CASE(OP_JUMP_IF_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

#undef COMPARE_JUMP

      VM_CASE(OP_LOOP) {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        VM_NEXT();
      }

      VM_CASE(OP_RETURN) {
        Value result = csVMPop();

        /* Anything this frame's locals were captured into has to move to the
         * heap before the slots are reused. */
        closeUpvalues(frame->slots);
        vm.frameCount--;

        if (vm.frameCount == 0) {
          csVMPop(); /* the script function itself */
          return CS_OK;
        }

        /* Discard the whole window, then leave the result where the callee was. */
        vm.stackTop = frame->slots;
        csVMPush(result);
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

#ifndef CS_COMPUTED_GOTO
      default:
        /* Unreachable: the only producer of bytecode is our own compiler. The
         * computed-goto path has no equivalent guard — an out-of-range byte
         * would index past the table — which is the price of the faster
         * dispatch, and is acceptable because chunks never come from disk. */
        csVMRuntimeError("unknown opcode %d", instruction);
        return CS_RUNTIME_ERROR;
#endif

  VM_END

#undef VM_END
#undef VM_NEXT
#undef VM_CASE
#undef VM_BEGIN
#undef VM_PROFILE_STEP
#undef VM_TRACE_STEP
#undef BINARY_NUMERIC_OP
#undef READ_STRING
#undef READ_CONSTANT
#undef READ_SHORT
#undef READ_BYTE
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

  /* Static checking sits between parsing and code generation: it needs the
   * whole tree, and the compiler benefits from the types it resolves. */
  if (!csTypeCheck(program, &diag)) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

#ifdef CS_DEBUG_PRINT_AST
  csAstPrint(program);
#endif

  ObjFunction *script = csCompile(program, &diag);
  /* The AST is only needed to produce bytecode, so it goes as soon as it has. */
  csAstArenaFree(&arena);
  if (script == NULL) return CS_COMPILE_ERROR;

#ifdef CS_DEBUG_PRINT_CODE
  csDisassembleChunk(&script->chunk, sourceName);
#endif

  /* The script is itself a function, so running it is just a call. Pushing the
   * closure first keeps it reachable while callClosure allocates nothing but
   * still leaves it rooted through the frame. */
  csPushTempRoot((Obj *)script);
  ObjClosure *closure = csClosureNew(script);
  csPopTempRoot();

  csVMPush(OBJ_VAL(closure));
  callClosure(closure, 0);

  InterpretResult result = run();
  resetStack();
  return result;
}
