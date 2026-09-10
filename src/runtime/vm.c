/* vm.c — the interpreter, in one translation unit and several files.
 *
 * There is one global VM. It owns every heap object, the intern pool and the
 * globals map, which is also what makes it the collector's root set.
 *
 * The helpers and the opcode bodies are `#include`d rather than linked, and
 * that is measured rather than habitual: they are the interpreter's hot path,
 * and the compiler inlines and specialises them into the dispatch loop.
 * Compiling them as translation units of their own cost 13-49% on the
 * interpreter benchmarks, because a cross-unit call cannot be inlined — and an
 * opcode body cannot become a function at all, since each ends in VM_NEXT(),
 * a jump to a label in run(). So the unit is one and the files are many.
 *
 * vm_fiber.c and vm_event.c are the exception: `await` and the microtask queue
 * are not on the hot path and link normally.
 */
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/debug.h"
#include "cscript/jit.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/parser.h"
#include "cscript/typecheck.h"
#include "cscript/vm.h"
#include "runtime/vm_internal.h"

VM vm;

#include "runtime/vm_state.inc"
#include "runtime/vm_number.inc"
#include "runtime/vm_property.inc"
#include "runtime/vm_call.inc"
#include "runtime/vm_invoke.inc"
#include "runtime/vm_iterate.inc"
#include "runtime/vm_upvalue.inc"
#include "runtime/vm_throw.inc"
#include "runtime/vm_cache_miss.inc"

/* Executes until the frame stack unwinds back to `baseFrame`.
 *
 * The top level passes 0, so the loop ends when the script returns. A native
 * calling back into user code passes the depth it started at, which turns this
 * into a nested interpreter that returns control as soon as that one call is
 * finished — without which `map`, `filter` and a `sort` comparator could not
 * run user code at all. */
InterpretResult run(int baseFrame) {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

/* `frame` is cached in a local rather than re-read from vm.frames each time;
 * it is refreshed on every call and return. */
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() \
  (frame->closure->function->chunk.constants.values[READ_SHORT()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define READ_PROPERTY_CACHE() \
  (&frame->closure->function->chunk.propertyCaches[READ_SHORT()])
#define READ_GLOBAL_CACHE() \
  (&frame->closure->function->chunk.globalCaches[READ_SHORT()])

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

/* The same, but a BigInt on either side takes the exact path instead. `code`
 * is what bigintArithmetic() switches on; it differs from the operator only
 * for `**`, which is not a character. */
#define BINARY_ARITHMETIC_OP(op, code)                                      \
  do {                                                                      \
    if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {               \
      if (!bigintArithmetic(code, #op)) return CS_RUNTIME_ERROR;            \
      break;                                                                \
    }                                                                       \
    BINARY_NUMERIC_OP(NUMBER_VAL, op);                                      \
  } while (false)

/* Ordering, where a BigInt and a number do mix. */
#define BINARY_COMPARE_OP(op)                                               \
  do {                                                                      \
    if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {               \
      int order = 0;                                                        \
      OrderResult ordering = comparingExactly(peekStack(1), peekStack(0),   \
                                              &order);                      \
      if (ordering == ORDER_INVALID) return CS_RUNTIME_ERROR;               \
      vm.stackTop -= 2;                                                     \
      /* Unordered means NaN, and every comparison with NaN is false —      \
       * including `>=`, which is why this is not `!(...)`. */              \
      csVMPush(BOOL_VAL(ordering == ORDER_KNOWN && (order op 0)));          \
      break;                                                                \
    }                                                                       \
    BINARY_NUMERIC_OP(BOOL_VAL, op);                                        \
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

/* A call may have failed because something inside it threw to a handler this
 * loop owns; if so, take the throw here rather than aborting. */
#define HANDLE_FAILED_CALL()                                            \
  do {                                                                  \
    if (!vm.hasPendingException) return CS_RUNTIME_ERROR;               \
    vm.hasPendingException = false;                                     \
    Value pending = vm.pendingException;                                \
    switch (performThrow(pending, baseFrame, &frame)) {                 \
      case THROW_HANDLED: break;                                        \
      case THROW_PROPAGATE: return CS_RUNTIME_ERROR;                    \
      case THROW_UNCAUGHT:                                              \
        return uncaught(pending);                                       \
    }                                                                   \
  } while (false)

      /* The opcode bodies, by role. Included rather than called: each ends in
       * VM_NEXT(), which under computed-goto dispatch is a jump to a label in
       * this function — so a case cannot become a function without giving up
       * the dispatch strategy, and giving that up is a measured loss. What
       * moved is the text; the loop is exactly as it was. */
#include "runtime/vm_ops_value.inc"
#include "runtime/vm_ops_iterate.inc"
#include "runtime/vm_ops_object.inc"
#include "runtime/vm_ops_build.inc"
#include "runtime/vm_ops_class.inc"
#include "runtime/vm_ops_arith.inc"

  VM_END

#undef VM_END
#undef VM_NEXT
#undef VM_CASE
#undef VM_BEGIN
#undef HANDLE_FAILED_CALL
#undef VM_PROFILE_STEP
#undef VM_TRACE_STEP
#undef BINARY_NUMERIC_OP
#undef BINARY_ARITHMETIC_OP
#undef BINARY_COMPARE_OP
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

/* Everything up to the bytecode, which is all `--check` wants and the front
 * half of what a run wants. `script` receives the compiled top level, or NULL
 * when the caller only asked whether it compiles. */
static InterpretResult compileSource(const char *source, const char *sourceName,
                                     ObjFunction **script) {
  Diagnostics diag;
  csDiagnosticsInit(&diag, source, sourceName);
  vm.sourceName = sourceName;

  if (csDumping(CS_DUMP_TOKENS)) {
    csLexerDumpTokens(source, &diag);
    csDiagnosticsInit(&diag, source, sourceName); /* reset after the dry run */
  }

  AstArena arena;
  csAstArenaInit(&arena);

  AstNode *program = csParse(source, &arena, &diag);
  if (program == NULL) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

  /* Code with no file of its own still resolves its imports, against the
   * working directory. */
  if (!csModuleLoadImports(program, vm.mainModule->path->chars, &diag)) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

  /* Static checking sits between parsing and code generation: it needs the
   * whole tree, and the compiler benefits from the types it resolves. */
  if (!csTypeCheck(program, &diag)) {
    csAstArenaFree(&arena);
    return CS_COMPILE_ERROR;
  }

  if (csDumping(CS_DUMP_AST)) csAstPrint(program);

  ObjFunction *compiled = csCompile(program, vm.mainModule, &diag);
  /* The AST is only needed to produce bytecode, so it goes as soon as it has. */
  csAstArenaFree(&arena);
  if (compiled == NULL) return CS_COMPILE_ERROR;

  if (csDumping(CS_DUMP_BYTECODE)) csDisassembleChunk(&compiled->chunk, sourceName);

  if (script != NULL) *script = compiled;
  return CS_OK;
}

InterpretResult csCheck(const char *source, const char *sourceName) {
  return compileSource(source, sourceName, NULL);
}

void csVMSetScriptArgs(const char *executable, const char *script,
                       const char *const *args, int count) {
  Value holder;
  ObjString *name = csStringCopy("process", 7);
  csPushTempRoot((Obj *)name);
  bool found = csTableGet(&vm.builtins, name, &holder);
  csPopTempRoot();
  if (!found || !IS_OBJECT(holder)) return;

  ObjArray *argv = csArrayNew();
  csPushTempRoot((Obj *)argv);

  /* Node's shape, because a program that reads this runs under Node too: the
   * executable, then the script, then what the user passed. A `-e` one-liner
   * and the REPL have no script, and Node leaves that slot out as well. */
  const char *leading[2] = {executable, script};
  for (int i = 0; i < 2; i++) {
    if (leading[i] == NULL) continue;
    ObjString *entry = csStringCopy(leading[i], (int)strlen(leading[i]));
    csPushTempRoot((Obj *)entry);
    csValueArrayWrite(&argv->elements, OBJ_VAL(entry));
    csPopTempRoot();
  }
  for (int i = 0; i < count; i++) {
    ObjString *entry = csStringCopy(args[i], (int)strlen(args[i]));
    csPushTempRoot((Obj *)entry);
    csValueArrayWrite(&argv->elements, OBJ_VAL(entry));
    csPopTempRoot();
  }

  csObjectSetProperty(AS_OBJECT(holder), "argv", OBJ_VAL(argv));
  csPopTempRoot();
}

InterpretResult csInterpret(const char *source, const char *sourceName) {
  ObjFunction *script = NULL;
  InterpretResult compiled = compileSource(source, sourceName, &script);
  if (compiled != CS_OK) return compiled;

  InterpretResult pending = csVMRunPendingModules();
  if (pending != CS_OK) return pending;

  InterpretResult result = csVMRunBody(script);
  if (result != CS_OK) return result;
  return csVMRunEventLoop();
}

/* A top level that awaits.
 *
 * `await` suspends a fiber, and only an async body has one — so a file with a
 * top-level await runs as an async call and the loop is driven here until it
 * settles. Driving it *here* rather than leaving it to the end is what keeps
 * imports in order: a module that awaits has finished before the module that
 * imported it starts, which is the guarantee the whole loader is built on. */
static InterpretResult runBodyAsync(ObjClosure *closure) {
  csVMPush(OBJ_VAL(closure));
  if (!callAsyncFunction(closure, 0)) {
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  ObjPromise *promise = AS_PROMISE(csVMPop());
  csPushTempRoot((Obj *)promise);
  /* Whatever it settles as is reported below, so the watchdog must not report
   * it first and count it twice. */
  promise->handled = true;

  InterpretResult result = csVMRunEventLoop();
  csPopTempRoot();
  if (result != CS_OK) {
    resetStack();
    return result;
  }

  if (promise->state == PROMISE_REJECTED) {
    fflush(stdout);
    size_t length = 0;
    char *text = csValueInspect(promise->value, &length);
    fprintf(stderr, "cscript: uncaught error at the top level: %s\n",
            text != NULL ? text : "<unprintable>");
    free(text);
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  if (promise->state == PROMISE_PENDING) {
    /* Nothing is left to run and it never settled, so nothing ever will. */
    csVMRuntimeError("a top-level 'await' is waiting for something that will "
                     "never happen");
    resetStack();
    return CS_RUNTIME_ERROR;
  }

  resetStack();
  return CS_OK;
}

InterpretResult csVMRunBody(ObjFunction *body) {
  /* A top level is itself a function, so running it is just a call. Pushing
   * the closure first keeps it reachable while callClosure allocates nothing
   * but still leaves it rooted through the frame. */
  csPushTempRoot((Obj *)body);
  ObjClosure *closure = csClosureNew(body);
  csPopTempRoot();

  if (body->isAsync) return runBodyAsync(closure);

  csVMPush(OBJ_VAL(closure));
  callClosure(closure, 0);

  InterpretResult result = run(0);
  resetStack();
  return result;
}










InterpretResult csVMRunPendingModules(void) {
  /* Dependency order, so everything a module imports has already run by the
   * time it starts. The list is cleared as it goes rather than at the end, so
   * a module that throws does not leave the rest queued behind it. */
  for (int i = 0; i < vm.pendingCount; i++) {
    ObjModule *module = vm.pending[i];
    if (module->executed) continue;

    ObjFunction *body = module->body;
    module->executed = true;
    module->body = NULL;

    InterpretResult result = csVMRunBody(body);
    if (result != CS_OK) {
      vm.pendingCount = 0;
      return result;
    }
  }
  vm.pendingCount = 0;
  return CS_OK;
}





bool csVMCallAdapted(Value callee, Value *args, int available, Value *result) {
  int wanted = available;
  if (IS_CLOSURE(callee)) {
    wanted = AS_CLOSURE(callee)->function->arity;
  } else if (IS_NATIVE(callee) && AS_NATIVE(callee)->arity >= 0) {
    wanted = AS_NATIVE(callee)->arity;
  }

  csVMPush(callee);
  for (int i = 0; i < wanted; i++) {
    csVMPush(i < available ? args[i] : UNDEFINED_VAL);
  }
  return csVMCallCallback(callee, wanted, result);
}

bool csVMCallCallback(Value callee, int argCount, Value *result) {
  /* A native calls into user code with the callee and its arguments already
   * pushed, exactly as OP_CALL would leave them. */
  int baseFrame = vm.frameCount;

  if (IS_NATIVE(callee)) {
    /* No frame to run — the native answers directly. */
    if (!callNative(AS_NATIVE(callee), UNDEFINED_VAL, argCount)) return false;
    *result = csVMPop();
    return true;
  }

  if (IS_BOUND_METHOD(callee)) {
    /* Unwrap once and re-enter, so a bound method reached through a callback
     * takes exactly the same path as one called directly. */
    ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
    if (bound->method->type == OBJ_NATIVE) {
      if (!callNative((ObjNative *)bound->method, bound->receiver, argCount)) return false;
      *result = csVMPop();
      return true;
    }
    vm.stackTop[-argCount - 1] = bound->receiver;
    /* The receiver is in place, so re-entering must not blank it. A generator
     * or an async body pushes no frame here, so those take the ordinary path,
     * which puts the receiver where their fiber's slot 0 will be. */
    ObjClosure *method = bound->method->type == OBJ_CLOSURE
                             ? (ObjClosure *)bound->method
                             : NULL;
    if (method != NULL && !method->function->isGenerator &&
        !method->function->isAsync) {
      return csVMCallCallbackWithReceiver(OBJ_VAL(method), argCount, result);
    }
    return csVMCallCallback(OBJ_VAL(bound->method), argCount, result);
  }

  if (!IS_CLOSURE(callee)) {
    csVMRuntimeError("%s is not a function", csValueTypeName(callee));
    return false;
  }

  /* Same again: a generator call pushes no frame either, because none of the
   * body runs until something pulls. */
  if (AS_CLOSURE(callee)->function->isGenerator) {
    if (!callGeneratorFunction(AS_CLOSURE(callee), argCount)) return false;
    *result = csVMPop();
    return true;
  }

  /* An async function returns its promise without pushing a frame, so there is
   * no nested loop to run — it either finished or is suspended already. */
  if (AS_CLOSURE(callee)->function->isAsync) {
    if (!callAsyncFunction(AS_CLOSURE(callee), argCount)) return false;
    *result = csVMPop();
    return true;
  }

  if (!callClosure(AS_CLOSURE(callee), argCount)) return false;

  /* Runs until that one call returns, leaving its result on the stack. */
  if (run(baseFrame) != CS_OK) return false;

  *result = csVMPop();
  return true;
}

/* The same, for a call whose slot 0 already holds the receiver. `new` on a
 * plain function needs it: the object being built is there, and blanking it
 * the way an ordinary call does would take `this` away from the constructor. */
bool csVMCallCallbackWithReceiver(Value callee, int argCount, Value *result) {
  int baseFrame = vm.frameCount;
  if (!csVMCallClosureWith(AS_CLOSURE(callee), argCount, true)) return false;
  if (run(baseFrame) != CS_OK) return false;
  *result = csVMPop();
  return true;
}
