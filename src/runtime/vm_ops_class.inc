/* vm_ops_class.inc — classes, `new`, `super`, modules and suspension.
 *
 * A class is built once, at its declaration: OP_CLASS makes it and the member
 * opcodes hang closures off it while it sits on the stack. `super` is reached
 * through a hidden local the method captured, so it resolves by the class the
 * method was *written* in rather than the class of the receiver.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */
      VM_CASE(OP_CLASS) {
        csVMPush(OBJ_VAL(csClassNew(READ_STRING())));
        VM_NEXT();
      }

      VM_CASE(OP_INHERIT) {
        Value superclass = peekStack(1);
        if (!IS_CLASS(superclass)) {
          csVMRuntimeError("a class can only extend another class, got %s",
                           csValueTypeName(superclass));
          return CS_RUNTIME_ERROR;
        }
        AS_CLASS(peekStack(0))->superclass = AS_CLASS(superclass);
        /* The superclass stays behind as the hidden local that `super` reads. */
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_METHOD)
      VM_CASE(OP_STATIC_METHOD) {
        ObjString *name = READ_STRING();
        ObjClass *klass = AS_CLASS(peekStack(1));
        /* Both stay on the stack across the insert, which can allocate. */
        csTableSet(instruction == OP_METHOD ? &klass->methods : &klass->statics,
                   name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_CLASS_MEMBER) {
        uint8_t kind = READ_BYTE();
        Value value = peekStack(0);
        ObjClass *klass = AS_CLASS(peekStack(2));

        ObjString *name = objectKeyFor(peekStack(1));
        if (name == NULL) {
          csVMRuntimeError("out of memory building a member name");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)name);

        Table *into = kind == 0   ? &klass->methods
                      : kind == 1 ? &klass->statics
                      : kind == 2 ? &klass->getters
                      : kind == 3 ? &klass->setters
                      : kind == 5 ? &klass->staticGetters
                      : kind == 6 ? &klass->staticSetters
                                  : &klass->statics;
        csTableSet(into, name, value);
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_GETTER)
      VM_CASE(OP_SETTER)
      VM_CASE(OP_STATIC_GETTER)
      VM_CASE(OP_STATIC_SETTER) {
        ObjString *name = READ_STRING();
        ObjClass *klass = AS_CLASS(peekStack(1));
        Table *into = instruction == OP_GETTER          ? &klass->getters
                      : instruction == OP_SETTER        ? &klass->setters
                      : instruction == OP_STATIC_GETTER ? &klass->staticGetters
                                                        : &klass->staticSetters;
        csTableSet(into, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_STATIC_FIELD) {
        ObjString *name = READ_STRING();
        csTableSet(&AS_CLASS(peekStack(1))->statics, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_CONSTRUCTOR) {
        AS_CLASS(peekStack(1))->initializer = AS_CLOSURE(peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_FIELD_INIT) {
        AS_CLASS(peekStack(1))->fieldInit = AS_CLOSURE(peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_NEW) {
        int argCount = READ_BYTE();
        Value target = peekStack(argCount);

        /* A built-in constructor is an ordinary native that returns the thing
         * it made, so `new Promise(executor)` and `Promise(executor)` are the
         * same call. There is no separate construction protocol to write. */
        if (IS_NATIVE(target)) {
          if (!callNative(AS_NATIVE(target), UNDEFINED_VAL, argCount)) {
            HANDLE_FAILED_CALL();
          }
          VM_NEXT();
        }

        /* A constructor function: `function Point(x) { this.x = x }`. What
         * makes it one is the `new`, not anything about how it was written —
         * which is JavaScript's rule exactly. The object being built inherits
         * from the function's `prototype`, so behaviour written there is
         * shared by everything the function constructs. */
        if (IS_CLOSURE(target)) {
          ObjClosure *constructor = AS_CLOSURE(target);
          if (constructor->function->isGenerator || constructor->function->isAsync) {
            csVMRuntimeError("'new' needs a class or an ordinary function, and "
                             "%s is neither",
                             constructor->function->isGenerator ? "a generator"
                                                                : "an async function");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }

          ObjObject *built = csObjectNew(constructor->function->name != NULL
                                             ? constructor->function->name->chars
                                             : "Object");
          csPushTempRoot((Obj *)built);
          built->builtByConstructor = constructor->function->name != NULL;
          built->prototype = csClosurePrototype(constructor);
          /* Where the class path puts the instance: slot 0, which the body
           * reads as `this`. */
          vm.stackTop[-argCount - 1] = OBJ_VAL(built);

          /* Run to completion in a loop of its own, because what the call
           * answers with decides the result: JavaScript lets a constructor
           * return a different object, and discards anything that is not one.
           * That choice cannot be made by leaving the frame to unwind. */
          Value returned;
          if (constructorAnsweredByCompiler(built, constructor, argCount,
                                            &returned)) {
            csPopTempRoot();
            csVMPush(IS_OBJECT(returned) ? returned : OBJ_VAL(built));
            VM_NEXT();
          }

          vm.pendingNewTarget = target;
          bool ok = csVMCallCallbackWithReceiver(OBJ_VAL(constructor), argCount,
                                                 &returned);
          vm.pendingNewTarget = UNDEFINED_VAL;
          csPopTempRoot();
          if (!ok) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          csVMPush(IS_OBJECT(returned) ? returned : OBJ_VAL(built));
          VM_NEXT();
        }

        if (!IS_CLASS(target)) {
          csVMRuntimeError("'new' needs a class or a function, got %s",
                           csValueTypeName(target));
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }

        ObjClass *klass = AS_CLASS(target);
        ObjObject *instance = csInstanceNew(klass);
        /* The class slot becomes the instance, which puts it exactly where a
         * constructor's slot 0 — its `this` — will be. */
        vm.stackTop[-argCount - 1] = OBJ_VAL(instance);

        ObjClass *owner = findConstructorOwner(klass);

        if (owner == NULL) {
          /* Nothing in the hierarchy declares a constructor, so all there is
           * to do is initialise the fields. */
          if (argCount != 0) {
            csVMRuntimeError("%s has no constructor but was given %d argument%s",
                             klass->name->chars, argCount, argCount == 1 ? "" : "s");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          if (!runFieldInitializers(klass, NULL, OBJ_VAL(instance))) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          /* The instance is already sitting where the result belongs. */
          VM_NEXT();
        }

        /* Classes between the instance's own and the one that owns the
         * constructor have implicit constructors, whose only job is to run
         * their fields — after the inherited constructor returns. */
        bool pendingFields = false;
        for (ObjClass *c = klass; c != owner; c = c->superclass) {
          if (c->fieldInit != NULL) {
            pendingFields = true;
            break;
          }
        }

        if (!pendingFields) {
          /* The common shape. A constructor returns `this`, so the frame it
           * leaves behind is the instance — no opcode needed to recover it.
           *
           * Compiled code leaves no frame, so it pushes the instance itself.
           * What such a body answers with is discarded on purpose: a class
           * constructor's result is always the instance, which is the rule the
           * frame version relies on too. */
          Value ignoredResult;
          if (constructorAnsweredByCompiler(instance, owner->initializer,
                                            argCount, &ignoredResult)) {
            csVMPush(OBJ_VAL(instance));
            VM_NEXT();
          }

          vm.pendingNewTarget = target;
          if (!callMethod(owner->initializer, argCount)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          VM_NEXT();
        }

        /* Otherwise the constructor has to finish before those fields run, so
         * it runs in a loop of its own and the fields follow it. */
        csPushTempRoot((Obj *)instance);
        Value ignored;
        vm.pendingNewTarget = target;
        bool ok = csVMCallCallback(OBJ_VAL(owner->initializer), argCount, &ignored) &&
                  runFieldInitializers(klass, owner, OBJ_VAL(instance));
        csPopTempRoot();
        if (!ok) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        csVMPush(OBJ_VAL(instance));
        VM_NEXT();
      }

      VM_CASE(OP_GET_SUPER) {
        ObjString *name = READ_STRING();
        ObjClass *superclass = AS_CLASS(peekStack(0));

        /* `super.x` where the superclass defines a getter runs it, rather than
         * handing back something to call. */
        ObjClosure *getter = IS_CLASS(peekStack(1))
                                 ? NULL
                                 : csClassFindGetter(superclass, name);
        if (getter != NULL) {
          Value receiver = peekStack(1);
          vm.stackTop -= 2;
          csVMPush(receiver);
          Value value;
          if (!csVMCallCallback(OBJ_VAL(getter), 0, &value)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          csVMPush(value);
          VM_NEXT();
        }

        ObjClosure *method =
            findSuperMember(superclass, name, IS_CLASS(peekStack(1)));
        if (method == NULL) {
          csVMRuntimeError("class %s has no member '%s'", superclass->name->chars,
                           name->chars);
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        ObjBoundMethod *bound = csBoundMethodNew(peekStack(1), (Obj *)method);
        vm.stackTop -= 2;
        csVMPush(OBJ_VAL(bound));
        VM_NEXT();
      }

      VM_CASE(OP_SUPER_INVOKE) {
        ObjString *name = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(csVMPop());
        ObjClosure *method =
            findSuperMember(superclass, name, IS_CLASS(peekStack(argCount)));
        if (method == NULL) {
          csVMRuntimeError("class %s has no member '%s'", superclass->name->chars,
                           name->chars);
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* `this` is still sitting below the arguments, which is where the
         * method's slot 0 has to be. */
        if (!callMethod(method, argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_SUPER_CALL) {
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(csVMPop());
        ObjClosure *constructor = findConstructor(superclass);

        /* `this` sits under the arguments, and stays on the stack while the
         * initialisers run so the collector can still see it. */
        Value receiver = peekStack(argCount);

        if (constructor == NULL) {
          if (argCount != 0) {
            csVMRuntimeError("%s has no constructor but super() was given "
                             "%d argument%s",
                             superclass->name->chars, argCount,
                             argCount == 1 ? "" : "s");
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          /* A superclass with no constructor still has fields, and they are
           * what `super()` is there to run: JavaScript sets them in the
           * implicit constructor this stands in for. */
          if (!runFieldInitializers(superclass, NULL, receiver)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          /* Drop `this` along with the arguments and stand in for the value
           * the constructor would have returned. */
          vm.stackTop -= argCount + 1;
          csVMPush(UNDEFINED_VAL);
          VM_NEXT();
        }

        /* The constructor found may be further up than the superclass, and
         * every class in between has fields of its own that nothing else will
         * run — the same gap `new` handles for the classes under the one that
         * owns the constructor. */
        ObjClass *superOwner = csVMConstructorOwner(superclass);
        bool superPendingFields = false;
        for (ObjClass *c = superclass; c != superOwner; c = c->superclass) {
          if (c->fieldInit == NULL) continue;
          superPendingFields = true;
          break;
        }

        if (!superPendingFields) {
          if (!callMethod(constructor, argCount)) {
            HANDLE_FAILED_CALL();
            VM_NEXT();
          }
          frame = &vm.frames[vm.frameCount - 1];
          VM_NEXT();
        }

        /* Those fields have to land after the inherited constructor returns,
         * so it runs in a loop of its own and they follow it. The receiver is
         * already in slot 0 and must stay there — blanking it the way an
         * ordinary call does would take `this` away from the constructor. */
        csPushTempRoot(AS_OBJ(receiver));
        Value ignoredSuper;
        bool superOk =
            csVMCallCallbackWithReceiver(OBJ_VAL(constructor), argCount, &ignoredSuper) && runFieldInitializers(superclass, superOwner, receiver);
        csPopTempRoot();
        if (!superOk) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        /* `super(...)` is an expression, and what it answers is undefined. */
        csVMPush(UNDEFINED_VAL);
        VM_NEXT();
      }

      VM_CASE(OP_IN) {
        Value target = peekStack(0);
        Value key = peekStack(1);
        bool result;

        if (IS_OBJECT(target) && IS_SYMBOL(key)) {
          Value ignored;
          bool has = csObjectGetPrivate(AS_OBJECT(target), AS_SYMBOL(key)->key,
                                        &ignored);
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(has));
          VM_NEXT();
        }
        if (IS_OBJECT(target)) {
          ObjString *name = objectKeyFor(key);
          if (name == NULL) {
            csVMRuntimeError("out of memory converting a key for 'in'");
            return CS_RUNTIME_ERROR;
          }
          csPushTempRoot((Obj *)name);
          Value ignored;
          ObjObject *object = AS_OBJECT(target);
          result = csObjectGetInherited(object, name, &ignored) ||
                   (object->klass != NULL &&
                    csClassFindMethod(object->klass, name) != NULL);
          csPopTempRoot();
        } else if (IS_ARRAY(target)) {
          /* An array's own keys are its indices, so `2 in [a, b, c]` is true
           * and `3` is not — the same question `in` asks of an object. */
          ObjArray *array = AS_ARRAY(target);
          result = IS_NUMBER(key) && AS_NUMBER(key) >= 0 &&
                   AS_NUMBER(key) < (double)array->elements.count &&
                   AS_NUMBER(key) == (double)(long long)AS_NUMBER(key);
        } else {
          csVMRuntimeError("the right side of 'in' must be an object or an "
                           "array, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(result));
        VM_NEXT();
      }

      VM_CASE(OP_INSTANCEOF) {
        Value target = peekStack(0);
        Value value = peekStack(1);

        bool result;
        if (IS_CLOSURE(target)) {
          /* For a constructor function the question is the one JavaScript
           * asks: is the function's `prototype` anywhere in the value's chain?
           * A function nothing has been constructed from has no prototype
           * object yet, and nothing can be an instance of it. */
          ObjObject *prototype = AS_CLOSURE(target)->prototype;
          result = false;
          if (prototype != NULL && IS_OBJECT(value)) {
            for (ObjObject *at = AS_OBJECT(value)->prototype; at != NULL;
                 at = at->prototype) {
              if (at != prototype) continue;
              result = true;
              break;
            }
          }
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(result));
          VM_NEXT();
        }

        if (!IS_CLASS(target)) {
          /* A built-in constructor reports as "function", so the plain message
           * read "must be a class or a function, got function" — which names
           * the rule and denies it in the same breath. What is actually true:
           * a built-in carries no prototype object, so nothing can be shown to
           * have come from one. */
          if (IS_NATIVE(target)) {
            csVMRuntimeError("'%s' is a built-in, and 'instanceof' cannot be used "
                             "with one — ask Object.kindOf(value) instead",
                             AS_NATIVE(target)->name->chars);
          } else {
            csVMRuntimeError("the right side of 'instanceof' must be a class or a "
                             "function, got %s",
                             csValueTypeName(target));
          }
          return CS_RUNTIME_ERROR;
        }
        result = IS_OBJECT(value) && AS_OBJECT(value)->klass != NULL &&
                 csClassDescendsFrom(AS_OBJECT(value)->klass, AS_CLASS(target));
        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(result));
        VM_NEXT();
      }

      VM_CASE(OP_IMPORT_NAME) {
        ObjString *name = READ_STRING();
        ObjModule *module = AS_MODULE(peekStack(0));
        Value value;
        if (!csTableGet(&module->globals, name, &value)) {
          /* The compiler checked the export list, so reaching here means the
           * module did not get as far as defining it. */
          csVMRuntimeError("'%s' was not defined by '%s'", name->chars,
                           module->path->chars);
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_IMPORT_NAMESPACE) {
        Value namespaceValue;
        if (!buildNamespace(AS_MODULE(peekStack(0)), &namespaceValue)) {
          return CS_RUNTIME_ERROR;
        }
        csVMPop();
        csVMPush(namespaceValue);
        VM_NEXT();
      }

      VM_CASE(OP_YIELD) {
        ObjFiber *fiber = vm.currentFiber;
        if (fiber == NULL || fiber->generator == NULL) {
          csVMRuntimeError("'yield' outside a generator");
          return CS_RUNTIME_ERROR;
        }

        fiber->generator->yielded = csVMPop();
        /* The frame's ip is already past this instruction, so resuming lands
         * on the next one — and what `next(x)` pushes arrives exactly where
         * the yielded value was, which is what makes `const x = yield v` a
         * two-way exchange. */
        vm.fiberSuspended = true;
        vm.fiberYielded = true;
        return CS_OK;
      }

      VM_CASE(OP_AWAIT) {
        ObjFiber *fiber = vm.currentFiber;
        if (fiber == NULL) {
          csVMRuntimeError("'await' outside an async function");
          return CS_RUNTIME_ERROR;
        }

        Value awaited = peekStack(0);
        ObjPromise *promise;
        if (IS_PROMISE(awaited)) {
          promise = AS_PROMISE(awaited);
        } else {
          /* Awaiting a plain value still yields to the microtask queue, which
           * is what keeps `await 1` from running its continuation early. */
          promise = csPromiseNew();
        }

        /* Rooted before anything else: settling and registering both allocate,
         * and once the value comes off the stack this is the only reference. */
        csPushTempRoot((Obj *)promise);
        if (!IS_PROMISE(awaited)) csPromiseFulfill(promise, awaited);
        csVMPop();

        promise->handled = true;
        csPromiseAddReaction(promise, UNDEFINED_VAL, UNDEFINED_VAL, NULL);
        if (promise->state == PROMISE_PENDING) {
          promise->reactions[promise->reactionCount - 1].fiber = fiber;
        } else {
          vm.microtasks[vm.microtaskCount - 1].fiber = fiber;
        }
        csPopTempRoot();

        /* Hand control back to whoever started this fiber. The frame's ip is
         * already past this instruction, so resuming lands on the next one and
         * the awaited value arrives in the promise's place. */
        vm.fiberSuspended = true;
        return CS_OK;
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
