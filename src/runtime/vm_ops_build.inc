/* vm_ops_build.inc — building an object or an array, and calling.
 *
 * An object literal with a spread in it is built one entry at a time rather
 * than from a run of stack pairs, because `{ ...a, b: 1, ...c }` has to apply
 * them in order: a later key wins over an earlier spread, and a later spread
 * wins over both.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */
      VM_CASE(OP_DYNAMIC_IMPORT) {
        /* `import(specifier)` — a module named at run time.
         *
         * Reading and compiling a file is synchronous here, so the promise is
         * settled before it is handed back. That is a difference from
         * JavaScript only in *when* the work happens, not in what a program
         * can observe: the result is still a promise, so it is still reached
         * through `await` or `.then`, and the microtask that delivers it runs
         * in the same turn it would have anyway. */
        Value specifier = peekStack(0);
        if (!IS_STRING(specifier)) {
          csVMRuntimeError("import() expects a string, got %s",
                           csValueTypeName(specifier));
          return CS_RUNTIME_ERROR;
        }

        ObjPromise *promise = csPromiseNew();
        csVMPop();
        csVMPush(OBJ_VAL(promise));

        Value loaded;
        bool fatal = false;
        if (importAtRuntime(frame->closure->function->module, AS_STRING(specifier),
                            &loaded, &fatal)) {
          csPromiseFulfill(promise, loaded);
        } else if (fatal) {
          return CS_RUNTIME_ERROR;
        } else {
          /* A module that cannot be found or read rejects rather than stopping
           * the program, which is the whole point of asking for one at run
           * time. */
          csPromiseReject(promise, loaded);
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_NEW_TARGET)
        csVMPush(frame->newTarget);
        VM_NEXT();

      VM_CASE(OP_SET_PROTOTYPE) {
        /* `{ __proto__: base }`. The object being built is beneath the value. */
        Value value = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(1));

        if (IS_NULL(value) || IS_UNDEFINED(value)) {
          object->prototype = NULL;
        } else if (!IS_OBJECT(value)) {
          csVMRuntimeError("a prototype must be an object or null, got %s",
                           csValueTypeName(value));
          return CS_RUNTIME_ERROR;
        } else if (!csObjectSetPrototype(object, AS_OBJECT(value))) {
          csVMRuntimeError("that prototype is already in this object\'s chain, "
                           "which would make every lookup on it loop");
          return CS_RUNTIME_ERROR;
        }

        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_SET) {
        Value value = peekStack(0);
        Value keyValue = peekStack(1);
        ObjObject *object = AS_OBJECT(peekStack(2));

        if (IS_SYMBOL(keyValue)) {
          csObjectPutPrivate(object, AS_SYMBOL(keyValue)->key, value);
          vm.stackTop -= 2;
          VM_NEXT();
        }

        ObjString *key = objectKeyFor(keyValue);
        if (key == NULL) {
          csVMRuntimeError("out of memory building an object key");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)key);
        csObjectPut(object, key, value);
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_ACCESSOR) {
        uint8_t isGetter = READ_BYTE();
        Value closure = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(2));

        ObjString *name = objectKeyFor(peekStack(1));
        if (name == NULL) {
          csVMRuntimeError("out of memory building an accessor name");
          return CS_RUNTIME_ERROR;
        }
        csPushTempRoot((Obj *)name);

        /* One class per object that needs one, made on demand. The property
         * paths already consult `klass` for accessors, so nothing else has to
         * learn about this. */
        if (object->klass == NULL) {
          object->klass = csClassNew(object->name);
          object->klass->isAccessorHolder = true;
        }
        csTableSet(isGetter ? &object->klass->getters : &object->klass->setters,
                   name, closure);

        /* The name also takes a slot, holding the stand-in, so that it is
         * enumerated in the order it was written. Shape mode has to go with
         * it: the read fast path would hand the stand-in back as a value. */
        csObjectLeaveShapeMode(object);
        csObjectPut(object, name, OBJ_VAL(vm.accessorMarker));
        csPopTempRoot();

        vm.stackTop -= 2;
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_MERGE) {
        Value source = peekStack(0);
        ObjObject *object = AS_OBJECT(peekStack(1));

        if (IS_OBJECT(source)) {
          ObjObject *from = AS_OBJECT(source);
          /* By index rather than by iterator, because csObjectPut on the
           * target cannot change the source. */
          for (int i = 0; i < csObjectCount(from); i++) {
            ObjString *key = csObjectKeyAt(from, i);
            if (!csObjectIsEnumerable(from, key)) continue;
            Value value;
            if (!csVMReadOwnProperty(from, key, &value)) return CS_RUNTIME_ERROR;
            csObjectPut(object, key, value);
          }
        } else if (IS_ARRAY(source)) {
          /* `{ ...[a, b] }` is `{ 0: a, 1: b }` — the indices are the keys. */
          ObjArray *array = AS_ARRAY(source);
          for (int i = 0; i < array->elements.count; i++) {
            char digits[16];
            int length = snprintf(digits, sizeof digits, "%d", i);
            ObjString *key = csStringCopy(digits, length);
            csPushTempRoot((Obj *)key);
            csObjectPut(object, key, array->elements.values[i]);
            csPopTempRoot();
          }
        } else if (!IS_NULL(source) && !IS_UNDEFINED(source)) {
          /* Spreading null or undefined contributes nothing, as in
           * JavaScript; anything else has no keys to contribute either. */
          csVMRuntimeError("cannot spread %s into an object",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_OBJECT_REST) {
        uint8_t taken = READ_BYTE();
        Value *names = vm.stackTop - taken;
        Value source = names[-1];

        if (!IS_OBJECT(source)) {
          csVMRuntimeError("cannot destructure %s as an object",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        ObjObject *rest = csObjectNew("Object");
        csPushTempRoot((Obj *)rest);

        ObjObject *from = AS_OBJECT(source);
        for (int i = 0; i < csObjectCount(from); i++) {
          ObjString *key = csObjectKeyAt(from, i);
          if (!csObjectIsEnumerable(from, key)) continue;

          bool named = false;
          for (int n = 0; n < taken && !named; n++) {
            named = IS_STRING(names[n]) && AS_STRING(names[n]) == key;
          }
          if (named) continue;

          Value value;
          if (!csVMReadOwnProperty(from, key, &value)) return CS_RUNTIME_ERROR;
          csObjectPut(rest, key, value);
        }

        csPopTempRoot();
        /* Replaces the source and the names, the shape OP_ARRAY_REST leaves. */
        vm.stackTop = names - 1;
        csVMPush(OBJ_VAL(rest));
        VM_NEXT();
      }

      VM_CASE(OP_TEMPLATE_STRINGS) {
        Value raw = peekStack(0);
        Value cooked = peekStack(1);
        csArrayPutExtra(AS_ARRAY(cooked), "raw", 3, raw);
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY_REST) {
        uint8_t from = READ_BYTE();
        /* Replaces the source on top with the slice, the same shape
         * OP_ITER_LENGTH uses, so no stack fixup is needed afterwards. */
        Value source = csVMPop();
        if (!IS_ARRAY(source)) {
          csVMRuntimeError("cannot destructure %s as an array",
                           csValueTypeName(source));
          return CS_RUNTIME_ERROR;
        }

        ObjArray *rest = csArrayNew();
        csPushTempRoot((Obj *)rest);
        ObjArray *array = AS_ARRAY(source);
        for (int i = from; i < array->elements.count; i++) {
          csValueArrayWrite(&rest->elements, array->elements.values[i]);
        }
        csPopTempRoot();

        csVMPush(OBJ_VAL(rest));
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
        if (!callValue(peekStack(argCount), argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* A user call pushed a frame, so the cached pointer is stale. */
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_CALL_SPREAD) {
        Value packed = csVMPop();
        if (!IS_ARRAY(packed)) {
          csVMRuntimeError("spread arguments must be an array");
          return CS_RUNTIME_ERROR;
        }

        ObjArray *args = AS_ARRAY(packed);
        int argCount = args->elements.count;
        if (argCount > UINT8_MAX) {
          csVMRuntimeError("too many arguments after spreading (limit %d)", UINT8_MAX);
          return CS_RUNTIME_ERROR;
        }

        /* The array is unreachable once popped, so root it while its elements
         * are copied onto the stack. */
        csPushTempRoot((Obj *)args);
        for (int i = 0; i < argCount; i++) csVMPush(args->elements.values[i]);
        csPopTempRoot();

        if (!callValue(peekStack(argCount), argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_INVOKE) {
        ObjString *name = READ_STRING();
        int argCount = READ_BYTE();
        if (!invokeMethod(peekStack(argCount), name, argCount)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* A method that turned out to be user code pushed a frame. */
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }
