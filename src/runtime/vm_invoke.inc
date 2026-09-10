/* vm_invoke.inc — `callee(...)` when the callee is not a plain closure.
 *
 * Six kinds of callable can follow a `(`, and a method call keeps the receiver
 * on the stack below the arguments rather than allocating a bound method per
 * call — which is the whole reason OP_INVOKE exists beside OP_CALL.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */


bool callValue(Value callee, int argCount) {
  if (IS_CLOSURE(callee)) {
    ObjClosure *closure = AS_CLOSURE(callee);
    /* A generator is a generator whether or not it is async: the call builds
     * the handle either way, and `async` only changes what `next()` answers
     * with. Checked first, because the async path would run the body. */
    if (closure->function->isGenerator) return callGeneratorFunction(closure, argCount);
    if (closure->function->isAsync) return callAsyncFunction(closure, argCount);

#ifdef CS_DEBUG_JIT
    /* Where the lowered IR covers a whole function, it runs instead of the
     * bytecode — which is what checks the lowering against the golden suite.
     *
     * It belongs here rather than in callClosure. That function's contract is
     * that a frame has been pushed, and csVMRunBody and csVMCallCallback both
     * start an interpreter loop immediately afterwards on the strength of it —
     * so replacing the frame with a value there segfaults on the entry call.
     * Returning a value instead is the *native* protocol, and this is the
     * level that already allows it. */
    {
      /* The boundary check happens here too, and has to: this path answers the
       * call without pushing a frame, so it never reaches csVMCheckArity —
       * and compiled code trusts a `number` annotation completely. Without
       * this, a value carried in through `any` was read as a double it is not,
       * and the same program answered differently with the compiler on. */
      if (!parameterTypesHold(closure->function, argCount)) return false;

      Value lowered;
      /* An ordinary call leaves the callee in slot 0, except where the body
       * says `this` — which the interpreter blanks, and so does this. */
      Value slotZero = closure->function->usesThis ? UNDEFINED_VAL : callee;
      if (csJitTryRun(closure->function, slotZero, vm.stackTop - argCount,
                      argCount, &lowered)) {
        vm.stackTop -= argCount + 1;
        csVMPush(lowered);
        return true;
      }
    }
#endif

    return callClosure(closure, argCount);
  }
  if (IS_NATIVE(callee)) return callNative(AS_NATIVE(callee), UNDEFINED_VAL, argCount);

  if (IS_BOUND_METHOD(callee)) {
    ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
    if (!spliceBoundArguments(bound, &argCount)) return false;
    if (bound->method->type == OBJ_NATIVE) {
      /* Natives already take a receiver, so a bound one is simply called with
       * the value it was bound to. */
      return callNative((ObjNative *)bound->method, bound->receiver, argCount);
    }
    /* The receiver goes back into slot 0, which is where the method's `this`
     * reads from — the same place OP_INVOKE would have left it. */
    vm.stackTop[-argCount - 1] = bound->receiver;
    return csVMCallClosureWith((ObjClosure *)bound->method, argCount, true);
  }

  if (IS_CLASS(callee)) {
    csVMRuntimeError("%s is a class; write 'new %s(...)' to construct one",
                     AS_CLASS(callee)->name->chars, AS_CLASS(callee)->name->chars);
    return false;
  }

  csVMRuntimeError("%s is not a function", csValueTypeName(callee));
  return false;
}

/* Calls a class method, leaving the receiver in slot 0 so `this` resolves to
 * it. Every other call path overwrites that slot with the callee, which is
 * harmless because nothing else names it — a method is the one case where the
 * body reads it. */
/* `super.name` in a static method means the superclass's static of that name,
 * and in an instance method its instance method. Nothing in the bytecode says
 * which, but the receiver does: a static is called with the class itself. */
static ObjClosure *findSuperMember(ObjClass *superclass, ObjString *name,
                                   bool isStatic) {
  if (!isStatic) return csClassFindMethod(superclass, name);
  for (ObjClass *klass = superclass; klass != NULL; klass = klass->superclass) {
    Value member;
    if (csTableGet(&klass->statics, name, &member)) {
      return (ObjClosure *)AS_OBJ(member);
    }
  }
  return NULL;
}

static bool callMethod(ObjClosure *method, int argCount) {
  /* An async method is still an async call: it needs its own fiber, and the
   * receiver already sits where that fiber's slot 0 will be. */
  if (method->function->isGenerator) return callGeneratorFunction(method, argCount);
  if (method->function->isAsync) return callAsyncFunction(method, argCount);
  return csVMCallClosureWith(method, argCount, true);
}

/* Does a call through a property have to hand this closure the receiver?
 *
 * In JavaScript `this` comes from the call site, not from how the function was
 * written: `o.f()` passes `o` whatever `f` is. A function written as a method
 * always wants it; a plain function wants it exactly when its body says
 * `this`, which is what makes `Point.prototype.sum = function () { ... }` work.
 * A function that never mentions `this` cannot tell the difference, so nothing
 * else has to change. */
static bool wantsReceiver(ObjClosure *closure) {
  return closure->function->isMethod || closure->function->usesThis;
}

/* The method table a receiver's built-ins live in, or NULL when it has none. */
static Table *methodTableFor(Value receiver) {
  if (IS_ARRAY(receiver)) return &vm.arrayMethods;
  if (IS_STRING(receiver)) return &vm.stringMethods;
  if (IS_PROMISE(receiver)) return &vm.promiseMethods;
  if (IS_MAP(receiver)) return AS_MAP(receiver)->isWeak ? &vm.weakMethods
                                                         : &vm.mapMethods;
  if (IS_GENERATOR(receiver)) return &vm.generatorMethods;
  if (IS_NUMBER(receiver)) return &vm.numberMethods;
  /* A closure or a bound method; a native's own statics are looked at first,
   * so `Number.isInteger` still wins over `Number.call`. */
  if (IS_CLOSURE(receiver) || IS_BOUND_METHOD(receiver) || IS_NATIVE(receiver)) {
    return &vm.functionMethods;
  }
  if (IS_REGEX(receiver)) return &vm.regexMethods;
  if (IS_DATE(receiver)) return &vm.dateMethods;
  if (IS_SYMBOL(receiver)) return &vm.symbolMethods;
  if (IS_BIGINT(receiver)) return &vm.bigintMethods;
  return NULL;
}

/* Looks `name` up on `receiver` and calls it, leaving the result where the
 * receiver was. Returns false with an error already reported.
 *
 * A plain object's own properties win over anything else, because a user
 * function stored on an object is exactly what it looks like. */
/* A constructor body answered by compiled code, leaving the stack as the
 * caller found it. `built` goes to slot 0, which is where the body's `this`
 * lives; what the body *returns* is handed back, because JavaScript lets a
 * constructor answer with a different object and the caller decides.
 *
 * `new` never reached the compiler at all before this, so a constructor could
 * compile and then only ever be interpreted — the same gap methods had. */
static bool constructorAnsweredByCompiler(ObjObject *built, ObjClosure *body,
                                          int argCount, Value *returned) {
#ifdef CS_DEBUG_JIT
  if (!csJitTryRun(body->function, OBJ_VAL(built), vm.stackTop - argCount,
                   argCount, returned)) {
    return false;
  }
  vm.stackTop -= argCount + 1;
  return true;
#else
  (void)built;
  (void)body;
  (void)argCount;
  (void)returned;
  return false;
#endif
}

/* A method answered by compiled code, or false to go the ordinary way.
 *
 * This is allowed *here* and not in callClosure, and the difference is the
 * contract. callClosure promises a frame has been pushed and its callers start
 * an interpreter loop on the strength of it; invokeMethod promises no such
 * thing — a native method already answers by leaving a value where the
 * receiver was, and both call sites in the interpreter recompute their frame
 * pointer afterwards either way.
 *
 * The receiver goes to slot 0, which is where a method's `this` lives and
 * where its property reads are checked. Passing it is the whole of what was
 * missing: the entry frame used to be built from the arguments alone, so a
 * method reading `this.x` failed its entry check against an undefined slot and
 * compiled code was never entered. */
static bool methodAnsweredByCompiler(Value receiver, ObjClosure *method,
                                     int argCount) {
#ifdef CS_DEBUG_JIT
  Value produced;
  if (!csJitTryRun(method->function, receiver, vm.stackTop - argCount, argCount,
                   &produced)) {
    return false;
  }
  vm.stackTop -= argCount + 1;
  csVMPush(produced);
  return true;
#else
  (void)receiver;
  (void)method;
  (void)argCount;
  return false;
#endif
}

static bool invokeMethod(Value receiver, ObjString *name, int argCount) {
  if (IS_OBJECT(receiver)) {
    Value method;
    /* `this.#run()` where `#run` is a field holding a function. Private fields
     * are not in the shape, so the ordinary lookup below would miss them and
     * fall through to the class's private methods. */
    if (name->length > 0 && name->chars[0] == '#' &&
        csObjectGetPrivate(AS_OBJECT(receiver), name, &method)) {
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }
    /* Own properties first, then the prototype chain — which is where a method
     * shared by every object built from the same prototype lives. */
    if (csObjectGetInherited(AS_OBJECT(receiver), name, &method)) {
      /* A property written as a method — `{ m() {} }` — keeps the receiver,
       * because that is what its `this` resolves to. Anything else is just a
       * function that happens to live on an object, and gets no receiver. */
      if (IS_CLOSURE(method) && wantsReceiver(AS_CLOSURE(method))) {
        if (methodAnsweredByCompiler(receiver, AS_CLOSURE(method), argCount)) {
          return true;
        }
        return callMethod(AS_CLOSURE(method), argCount);
      }

      /* Overwrite the receiver slot so the callee sits directly below its
       * arguments, which is the shape callValue expects. */
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }

    /* Only now the class chain, so a field holding a function still wins —
     * which also means `this` is bound for methods declared in a class body
     * and not for a function that merely lives on an object. */
    ObjObject *instance = AS_OBJECT(receiver);
    if (instance->klass != NULL) {
      ObjClosure *classMethod = csClassFindMethod(instance->klass, name);
      if (classMethod != NULL) {
        if (methodAnsweredByCompiler(receiver, classMethod, argCount)) return true;
        return callMethod(classMethod, argCount);
      }
    }

    csVMRuntimeError("'%s' has no method '%s'", instance->name->chars, name->chars);
    return false;
  }

  if (IS_CLASS(receiver)) {
    /* A static keeps the class in slot 0, so `this` inside one is the class. */
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value method;
      if (csTableGet(&klass->statics, name, &method)) {
        return callMethod((ObjClosure *)AS_OBJ(method), argCount);
      }
    }
    csVMRuntimeError("class %s has no static method '%s'",
                     AS_CLASS(receiver)->name->chars, name->chars);
    return false;
  }

  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    Value method;
    if (csObjectGet(AS_NATIVE(receiver)->statics, name, &method)) {
      vm.stackTop[-argCount - 1] = method;
      return callValue(method, argCount);
    }
    csVMRuntimeError("'%s' has no method '%s'", AS_NATIVE(receiver)->name->chars,
                     name->chars);
    return false;
  }

  Table *methods = methodTableFor(receiver);
  if (methods != NULL) {
    Value method;
    if (csTableGet(methods, name, &method)) {
      return callNative(AS_NATIVE(method), receiver, argCount);
    }
    /* typeof [] is "object", which would make the message confusing here. */
    csVMRuntimeError("%s has no method '%s'",
                     IS_ARRAY(receiver)     ? "array"
                     : IS_PROMISE(receiver) ? "a promise"
                                            : csValueTypeName(receiver),
                     name->chars);
    return false;
  }

  csVMRuntimeError("cannot call '%s' on %s", name->chars, csValueTypeName(receiver));
  return false;
}
