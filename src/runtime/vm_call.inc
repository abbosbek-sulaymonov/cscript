/* vm_call.inc — calling, and the arity a call has to satisfy.
 *
 * A parameter with a default is not among the arguments a call must supply, so
 * arity is a range rather than a number, and the frame is padded out before
 * the body runs — which is where the defaults are applied.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
/* An Error object with a formatted message, for something a program is meant
 * to catch rather than be stopped by. */
static Value csVMMakeError(const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);

  ObjObject *error = csObjectNew("Error");
  csPushTempRoot((Obj *)error);
  ObjString *name = csStringCopy("Error", 5);
  csPushTempRoot((Obj *)name);
  csObjectSetProperty(error, "name", OBJ_VAL(name));
  csPopTempRoot();
  ObjString *text = csStringCopy(message, (int)strlen(message));
  csPushTempRoot((Obj *)text);
  csObjectSetProperty(error, "message", OBJ_VAL(text));
  csPopTempRoot();
  csPopTempRoot();
  return OBJ_VAL(error);
}

/* Loads, compiles and runs a module named at run time, and answers its
 * namespace. False with an error reported.
 *
 * The bodies run as nested calls rather than through csVMRunBody, which resets
 * the stack when it finishes — right for a top level and fatal in the middle
 * of one. */
static bool importAtRuntime(ObjModule *from, ObjString *specifier, Value *out,
                            bool *fatal) {
  *fatal = false;

  char resolved[4096];
  const char *fromPath = from != NULL ? from->path->chars : "<main>";
  if (!csModuleResolve(fromPath, specifier->chars, resolved, sizeof resolved)) {
    *out = csVMMakeError("cannot find module '%s'%s", specifier->chars,
                         csModuleResolutionHint(specifier->chars));
    return false;
  }

  ObjModule *module = csModuleLoadResolved(resolved, specifier->chars, NULL, 0);
  if (module == NULL) {
    *out = csVMMakeError("cannot load module '%s'", specifier->chars);
    return false;
  }

  /* Everything the load queued, in dependency order — which for a module
   * already loaded is nothing at all, so importing one twice runs it once. */
  for (int i = 0; i < vm.pendingCount; i++) {
    ObjModule *pending = vm.pending[i];
    if (pending->executed) continue;

    ObjFunction *body = pending->body;
    pending->executed = true;
    pending->body = NULL;

    if (body->isAsync) {
      /* A top level that awaits has to settle before the namespace is worth
       * anything, and settling it means running the event loop from inside
       * this call. Naming that beats handing back a namespace of names that
       * are not there yet. */
      vm.pendingCount = 0;
      *out = csVMMakeError("'%s' has a top-level await, which import() cannot "
                           "wait for; import it statically instead",
                           csModuleDisplayPath(pending->path->chars));
      return false;
    }

    csPushTempRoot((Obj *)body);
    ObjClosure *closure = csClosureNew(body);
    csPopTempRoot();

    int baseFrame = vm.frameCount;
    csVMPush(OBJ_VAL(closure));
    if (!callClosure(closure, 0)) {
      vm.pendingCount = 0;
      *fatal = true;
      return false;
    }
    if (run(baseFrame) != CS_OK) {
      /* Something inside the module went wrong. A runtime error is not
       * catchable here, so it keeps travelling rather than becoming a
       * rejection that hides it. */
      vm.pendingCount = 0;
      *fatal = true;
      return false;
    }
    csVMPop(); /* what the body left behind */
  }
  vm.pendingCount = 0;

  return buildNamespace(module, out);
}

static bool concatenateOrAdd(void) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (IS_STRING(a) || IS_STRING(b)) {
    size_t aLength = 0;
    size_t bLength = 0;
    char *aText = csVMValueToText(a, &aLength);
    char *bText = csVMValueToText(b, &bLength);
    if (aText == NULL || bText == NULL) {
      free(aText);
      free(bText);
      /* Two different failures reach here: an allocation that could not be
       * made, and a user `toString` that threw. Reporting the first over the
       * second would replace a real error with a wrong one — and would lose
       * the throw, which an enclosing `try` was entitled to catch. */
      if (!vm.hasPendingException) {
        csVMRuntimeError("out of memory while concatenating strings");
      }
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

  if (IS_BIGINT(a) || IS_BIGINT(b)) return bigintArithmetic('+', "+");

  if (!IS_NUMBER(a) || !IS_NUMBER(b)) {
    csVMRuntimeError("cannot add %s and %s", csValueTypeName(a), csValueTypeName(b));
    return false;
  }

  csVMPop();
  csVMPop();
  csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
  return true;
}

/* Slides the arguments a call supplied up and drops `bind`'s in front of them.
 *
 * The stack already holds [callee][supplied…], and the callee's own slot is
 * about to be overwritten with the receiver, so the presets go exactly where
 * the arguments a direct call would have pushed first belong. */
static bool spliceBoundArguments(ObjBoundMethod *bound, int *argCount) {
  if (bound->presets == NULL || bound->presets->elements.count == 0) return true;

  int extra = bound->presets->elements.count;
  if (vm.stackTop + extra >= vm.stack + vm.stackCapacity) {
    csVMRuntimeError("call stack overflow while applying a bound function");
    return false;
  }

  Value *supplied = vm.stackTop - *argCount;
  memmove(supplied + extra, supplied, sizeof(Value) * (size_t)*argCount);
  for (int i = 0; i < extra; i++) supplied[i] = bound->presets->elements.values[i];

  vm.stackTop += extra;
  *argCount += extra;
  return true;
}

/* The contract at the gradual boundary.
 *
 * A `number` annotation is not advice: the compiler emits OP_ADD_NUM for
 * arithmetic on it and the JIT seeds the slot as a number and stops guarding.
 * The checker enforces the annotation wherever it can see the argument's type
 * — but `any` is assignable to everything by design, so a value that came in
 * through an untyped edge reaches the callee unexamined. Without this check it
 * would be read as a double it is not, and the answer would be quietly wrong
 * rather than a reported error.
 *
 * This is the one check type.h promised gradual typing would need. Only
 * `number` is checked, because `number` is the only annotation anything
 * currently generates code from — guarding a type nothing trusts would cost
 * without buying anything.
 *
 * Arguments the caller did not supply are not checked: those are about to be
 * padded with undefined and then filled in by the defaults prologue. */
static bool parameterTypesHold(ObjFunction *function, int argCount) {
  if (function->paramTypes == NULL) return true;

  int checked = argCount < function->paramCount ? argCount : function->paramCount;
  for (int i = 0; i < checked; i++) {
    if (function->paramTypes[i] != TYPE_NUMBER) continue;

    Value argument = vm.stackTop[i - argCount];
    if (IS_NUMBER(argument)) continue;

    const char *name =
        function->name != NULL ? function->name->chars : "<anonymous>";
    csVMRuntimeError("%s: argument %d is %s but the parameter is number", name,
                     i + 1, csValueTypeName(argument));
    return false;
  }
  return true;
}

/* Checks the argument count and pads the frame out to the parameter count.
 *
 * A parameter with a default is optional, so a call may supply fewer than
 * there are parameters — but the body still refers to every one of them by
 * slot, so the missing ones are pushed as undefined before the frame starts.
 * That is also what makes an argument written as `undefined` and one left out
 * the same thing, which is what JavaScript says. */
bool csVMCheckArity(ObjFunction *function, int *argCount) {
  if (!parameterTypesHold(function, *argCount)) return false;

  /* A rest parameter takes whatever is left, so there is no upper bound and
   * the leftovers become one array in its slot. */
  if (function->hasRest) {
    int fixed = function->paramCount - 1;
    if (*argCount < function->arity) {
      const char *name =
          function->name != NULL ? function->name->chars : "<anonymous>";
      csVMRuntimeError("%s expects at least %d argument%s but got %d", name,
                       function->arity, function->arity == 1 ? "" : "s", *argCount);
      return false;
    }

    while (*argCount < fixed) {
      csVMPush(UNDEFINED_VAL);
      (*argCount)++;
    }

    ObjArray *rest = csArrayNew();
    csPushTempRoot((Obj *)rest);
    for (int i = fixed; i < *argCount; i++) {
      csValueArrayWrite(&rest->elements, vm.stackTop[i - *argCount]);
    }
    csPopTempRoot();

    vm.stackTop -= *argCount - fixed;
    csVMPush(OBJ_VAL(rest));
    *argCount = fixed + 1;
    return true;
  }

  if (*argCount < function->arity || *argCount > function->paramCount) {
    const char *name =
        function->name != NULL ? function->name->chars : "<anonymous>";
    if (function->arity == function->paramCount) {
      csVMRuntimeError("%s expects %d argument%s but got %d", name, function->arity,
                       function->arity == 1 ? "" : "s", *argCount);
    } else {
      csVMRuntimeError("%s expects between %d and %d arguments but got %d", name,
                       function->arity, function->paramCount, *argCount);
    }
    return false;
  }

  while (*argCount < function->paramCount) {
    csVMPush(UNDEFINED_VAL);
    (*argCount)++;
  }
  return true;
}

/* Pushes a frame for a user function. The frame's window starts at the callee
 * itself, so slot 0 is the function and slots 1..arity are the arguments —
 * exactly where the compiler assigned the parameters. */
bool callClosure(ObjClosure *closure, int argCount) {
  return csVMCallClosureWith(closure, argCount, false);
}

/* Records what this call passed, for a function still being counted towards
 * the compile threshold.
 *
 * Only while it is being counted: once the decision is made the observation
 * has been used, and going on paying for it on every call would be a tax on
 * exactly the code the compiler is trying to speed up. A call that arrives
 * after that with a different type is caught by the entry guard instead. */
#ifdef CS_DEBUG_JIT
static void observeArguments(ObjFunction *function, int argCount) {
  if (function->paramCount == 0 || function->paramCount > 8) return;

  if (function->observedParams == NULL) {
    function->observedParams = (uint8_t *)calloc((size_t)function->paramCount, 1);
    if (function->observedParams == NULL) return;
  }

  for (int i = 0; i < function->paramCount; i++) {
    if (function->observedParams[i] == CS_PARAM_MIXED) continue;
    /* A call that did not supply this one says nothing about it: the defaults
     * prologue has not run yet, so the slot holds nothing worth reading. */
    if (i >= argCount) continue;

    bool isNumber = IS_NUMBER(vm.stackTop[i - argCount]);
    function->observedParams[i] = isNumber ? CS_PARAM_NUMBER : CS_PARAM_MIXED;
  }
}
#endif

/* `hasReceiver` says slot 0 already holds what `this` should be — a method's
 * receiver, or the object `new` is building. Without it the slot still holds
 * the callee, and a function that says `this` would see itself. JavaScript
 * answers `undefined` there in a module, and so does this. */
bool csVMCallClosureWith(ObjClosure *closure, int argCount, bool hasReceiver) {
  ObjFunction *function = closure->function;

  /* A method is never called without one: every path that reaches one puts the
   * receiver in slot 0 first, including a native that pushes it by hand before
   * running a getter. Only a plain function can arrive here with the callee
   * still sitting in the slot its `this` will read. */
  if (function->usesThis && !function->isMethod && !hasReceiver) {
    vm.stackTop[-argCount - 1] = UNDEFINED_VAL;
  }

  if (!csVMCheckArity(function, &argCount)) return false;

  if (vm.frameCount == vm.frameCapacity) {
    csVMRuntimeError("call stack overflow (limit %d frames)", vm.frameCapacity);
    return false;
  }

  /* A generator's body never runs here: the call builds the handle. */
  if (function->isGenerator) return callGeneratorFunction(closure, argCount);

#ifdef CS_DEBUG_JIT
  /* Before the tick, so the call that crosses the threshold is counted in what
   * the compiler is about to look at. */
  if (function->jitState == JIT_INTERPRETED) observeArguments(function, argCount);
#endif
  CS_JIT_TICK(function);

  /* Compiled code is *not* entered here, and the reason is this function's
   * contract: it promises a frame has been pushed, and csVMRunBody,
   * csVMCallCallback and csVMCallCallbackWithReceiver all start an interpreter
   * loop immediately afterwards on the strength of that promise. Answering
   * with a value instead is the native protocol, and callValue is the level
   * that allows it — which is where the entry lives.
   *
   * There used to be one here as well. Nothing caught it because reaching it
   * needed a function whose parameters were typed *and* which was called back
   * from a native — a comparator passed to `sort`, say — and no test had one
   * until parameter types started being guessed rather than declared. */
  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  /* Taken rather than read: the next call is a plain one unless OP_NEW sets it
   * again, which is what makes a constructor's own calls answer undefined the
   * way JavaScript's do. */
  frame->newTarget = vm.pendingNewTarget;
  vm.pendingNewTarget = UNDEFINED_VAL;
  return true;
}

/* `receiver` is the value a method was invoked on, or undefined for a plain
 * call. It sits on the stack below the arguments either way, so dropping
 * `argCount + 1` slots discards it along with them. */
bool callNative(ObjNative *native, Value receiver, int argCount) {
  if (native->arity >= 0 && argCount != native->arity) {
    csVMRuntimeError("%s expects %d argument%s but got %d", native->name->chars,
                     native->arity, native->arity == 1 ? "" : "s", argCount);
    return false;
  }

  Value result;
  if (!native->function(receiver, argCount, vm.stackTop - argCount, &result)) {
    return false;
  }

  /* Drop the arguments and the callee, then push the result in its place. */
  vm.stackTop -= argCount + 1;
  csVMPush(result);
  return true;
}
