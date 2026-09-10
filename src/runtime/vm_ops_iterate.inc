/* vm_ops_iterate.inc — indexing, iteration and destructuring.
 *
 * `for...of` is one opcode per step rather than the six it replaces, because a
 * generator cannot be driven by an index at all — asking one for its length
 * would mean running it to the end first, which is wrong for an infinite
 * sequence and wrong for the order side effects happen in.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */

      VM_CASE(OP_REGEX) {
        ObjString *source = READ_STRING();
        ObjString *flags = READ_STRING();
        ObjRegex *regex = csRegexObjectNew(source, flags);
        if (regex == NULL) return CS_RUNTIME_ERROR;
        csVMPush(OBJ_VAL(regex));
        VM_NEXT();
      }

      VM_CASE(OP_ITER_PREPARE) {
        uint8_t forAwait = READ_BYTE();
        /* Arrays and strings are already indexable, so the common case is a
         * type test and nothing else. */
        Value target = peekStack(0);

        /* An object that offers `Symbol.iterator` is replaced by whatever that
         * hands back, and the loop pulls from there — which is the whole of
         * what makes a user-defined type iterable. */
        Value iteratorMethod;
        if ((forAwait && findAsyncIteratorMethod(target, &iteratorMethod)) ||
            findIteratorMethod(target, &iteratorMethod)) {
          Value iterator;
          if (!callWithReceiver(iteratorMethod, target, NULL, 0, &iterator)) {
            return CS_RUNTIME_ERROR;
          }
          csVMPop();
          csVMPush(iterator);
          frame = &vm.frames[vm.frameCount - 1];
          VM_NEXT();
        }

        if (!IS_MAP(target)) VM_NEXT();
        if (AS_MAP(target)->isWeak) {
          csVMRuntimeError("a %s cannot be iterated: what is left in it depends "
                           "on when the collector last ran",
                           AS_MAP(target)->isSet ? "WeakSet" : "WeakMap");
          return CS_RUNTIME_ERROR;
        }

        ObjArray *items = csMapToArray(AS_MAP(target));
        csVMPop();
        csVMPush(OBJ_VAL(items));
        VM_NEXT();
      }

      VM_CASE(OP_ENUM_KEYS) {
        Value target = peekStack(0);
        ObjArray *keys = csArrayNew();
        csVMPush(OBJ_VAL(keys)); /* rooted while the copies below allocate */

        if (IS_OBJECT(target)) {
          ObjObject *object = AS_OBJECT(target);
          for (int i = 0; i < csObjectCount(object); i++) {
            ObjString *key = csObjectKeyAt(object, i);
            if (!csObjectIsEnumerable(object, key)) continue;
            csValueArrayWrite(&keys->elements, OBJ_VAL(key));
          }
        } else if (IS_ARRAY(target)) {
          /* JavaScript enumerates an array's indices, as strings. */
          for (int i = 0; i < AS_ARRAY(target)->elements.count; i++) {
            char digits[16];
            int length = snprintf(digits, sizeof digits, "%d", i);
            /* The new string is reachable from nothing while the array grows,
             * and growing allocates. */
            ObjString *index = csStringCopy(digits, length);
            csPushTempRoot((Obj *)index);
            csValueArrayWrite(&keys->elements, OBJ_VAL(index));
            csPopTempRoot();
          }
        } else if (!IS_NULL(target) && !IS_UNDEFINED(target)) {
          csVMRuntimeError("'for...in' needs an object or an array, got %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }

        vm.stackTop -= 2;
        csVMPush(OBJ_VAL(keys));
        VM_NEXT();
      }

      VM_CASE(OP_ITER_LENGTH) {
        Value iterable = csVMPop();
        if (IS_ARRAY(iterable)) {
          csVMPush(NUMBER_VAL(AS_ARRAY(iterable)->elements.count));
          VM_NEXT();
        }
        if (IS_STRING(iterable)) {
          csVMPush(NUMBER_VAL(AS_STRING(iterable)->length));
          VM_NEXT();
        }
        csVMRuntimeError("%s is not iterable", csValueTypeName(iterable));
        return CS_RUNTIME_ERROR;
      }

      VM_CASE(OP_JUMP_IF_ASYNC_ITER) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        /* An async generator, or any iterator object — the second may answer
         * `next()` with a promise or with a record, and the await that follows
         * handles both. */
        if ((IS_GENERATOR(top) && AS_GENERATOR(top)->isAsync) || IS_OBJECT(top)) {
          frame->ip += offset;
        }
        VM_NEXT();
      }

      VM_CASE(OP_ASYNC_NEXT) {
        Value target = csVMPop();
        if (IS_GENERATOR(target)) {
          csVMPush(OBJ_VAL(csGeneratorNextAsync(AS_GENERATOR(target), UNDEFINED_VAL)));
          VM_NEXT();
        }

        Value next;
        if (!IS_OBJECT(target) ||
            !csObjectGet(AS_OBJECT(target), csStringCopy("next", 4), &next)) {
          if (!IS_OBJECT(target) || AS_OBJECT(target)->klass == NULL ||
              (next = OBJ_VAL(csClassFindMethod(AS_OBJECT(target)->klass,
                                                csStringCopy("next", 4))),
               !IS_CLOSURE(next))) {
            csVMRuntimeError("an async iterator must have a 'next' method");
            return CS_RUNTIME_ERROR;
          }
        }

        Value answered;
        if (!callWithReceiver(next, target, NULL, 0, &answered)) {
          return CS_RUNTIME_ERROR;
        }
        csVMPush(answered);
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_ITER_UNPACK) {
        uint16_t offset = READ_SHORT();
        Value record = peekStack(0);
        if (!IS_OBJECT(record)) {
          csVMRuntimeError("an async iterator must answer with an object, got %s",
                           csValueTypeName(record));
          return CS_RUNTIME_ERROR;
        }

        Value done, produced;
        ObjObject *object = AS_OBJECT(record);
        if (!csObjectGet(object, csStringCopy("done", 4), &done)) done = BOOL_VAL(true);
        if (!csObjectGet(object, csStringCopy("value", 5), &produced)) {
          produced = UNDEFINED_VAL;
        }

        csVMPop();
        if (csValueIsTruthy(done)) {
          frame->ip += offset;
          VM_NEXT();
        }
        csVMPush(produced);
        VM_NEXT();
      }

      VM_CASE(OP_DESTRUCTURE_PREPARE) {
        uint8_t wanted = READ_BYTE();
        Value target = peekStack(0);
        if (IS_ARRAY(target) || IS_STRING(target)) VM_NEXT();

        /* A Map or a Set walks to what iterating it yields, the same
         * conversion `for...of` and a spread both make. */
        if (IS_MAP(target)) {
          if (AS_MAP(target)->isWeak) {
            csVMRuntimeError("a %s cannot be destructured: what is left in it "
                             "depends on when the collector last ran",
                             AS_MAP(target)->isSet ? "WeakSet" : "WeakMap");
            return CS_RUNTIME_ERROR;
          }
          ObjArray *items = csMapToArray(AS_MAP(target));
          csVMPop();
          csVMPush(OBJ_VAL(items));
          VM_NEXT();
        }

        Value source = target;
        if (!IS_GENERATOR(source)) {
          Value iteratorMethod;
          if (!findIteratorMethod(source, &iteratorMethod)) VM_NEXT();
          if (!callWithReceiver(iteratorMethod, source, NULL, 0, &source)) {
            return CS_RUNTIME_ERROR;
          }
        }

        csPushTempRoot(AS_OBJ(source));
        ObjArray *taken = csArrayNew();
        csPushTempRoot((Obj *)taken);
        for (int i = 0; wanted == 255 || i < wanted; i++) {
          Value produced;
          bool done;
          if (!iteratorStep(source, &produced, &done)) {
            csPopTempRoot();
            csPopTempRoot();
            return CS_RUNTIME_ERROR;
          }
          if (done) break;
          csValueArrayWrite(&taken->elements, produced);
        }
        csPopTempRoot();
        csPopTempRoot();

        csVMPop();
        csVMPush(OBJ_VAL(taken));
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

      VM_CASE(OP_ITER_STEP) {
        uint16_t offset = READ_SHORT();
        Value index = peekStack(0);
        Value iterable = peekStack(1);

        if (IS_ARRAY(iterable)) {
          /* The length is read each pass, so a body that appends is seen —
           * which is what the index-driven loop did before. */
          ObjArray *array = AS_ARRAY(iterable);
          int at = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (at >= array->elements.count) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(array->elements.values[at]);
          VM_NEXT();
        }

        if (IS_STRING(iterable)) {
          ObjString *string = AS_STRING(iterable);
          int at = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (at >= string->length) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(OBJ_VAL(csStringCopy(string->chars + at, 1)));
          VM_NEXT();
        }

        if (IS_GENERATOR(iterable)) {
          ObjGenerator *generator = AS_GENERATOR(iterable);
          Value produced;
          bool done;
          /* Resuming runs a nested interpreter on the generator's own stack.
           * It restores this one before returning, so `frame` still points
           * where it did. */
          if (!csGeneratorNext(generator, UNDEFINED_VAL, &produced, &done)) {
            return CS_RUNTIME_ERROR;
          }
          vm.stackTop -= 2;
          if (done) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(produced);
          VM_NEXT();
        }

        /* Anything left is either an iterator OP_ITER_PREPARE produced, or a
         * value that was never iterable. */
        if (IS_OBJECT(iterable)) {
          Value produced;
          bool done;
          if (!iteratorStep(iterable, &produced, &done)) return CS_RUNTIME_ERROR;
          vm.stackTop -= 2;
          frame = &vm.frames[vm.frameCount - 1];
          if (done) {
            frame->ip += offset;
            VM_NEXT();
          }
          csVMPush(produced);
          VM_NEXT();
        }

        csVMRuntimeError("%s is not iterable", csValueTypeName(iterable));
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
        } else if (IS_OBJECT(target) && IS_SYMBOL(index)) {
          csObjectPutPrivate(AS_OBJECT(target), AS_SYMBOL(index)->key, value);
        } else if (IS_OBJECT(target)) {
          ObjString *key = objectKeyFor(index);
          if (key == NULL) {
            csVMRuntimeError("out of memory converting an object key");
            return CS_RUNTIME_ERROR;
          }
          csPushTempRoot((Obj *)key); /* csObjectPut can collect */
          /* `console["log"] = f` is the same act as `console.log = f`. */
          if (AS_OBJECT(target)->frozen) {
            csPopTempRoot();
            csVMRuntimeError("'%s.%s' cannot be changed: the object is frozen",
                             AS_OBJECT(target)->name->chars, key->chars);
            return CS_RUNTIME_ERROR;
          }
          csObjectPut(AS_OBJECT(target), key, value);
          csPopTempRoot();
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
          Value key = entries[i * 2];
          if (IS_SYMBOL(key)) {
            csObjectPutPrivate(object, AS_SYMBOL(key)->key, entries[i * 2 + 1]);
            continue;
          }
          if (!IS_STRING(key)) {
            ObjString *converted = objectKeyFor(key);
            if (converted == NULL) {
              csVMRuntimeError("out of memory building an object key");
              return CS_RUNTIME_ERROR;
            }
            entries[i * 2] = OBJ_VAL(converted); /* keeps it rooted */
            key = entries[i * 2];
          }
          csObjectPut(object, AS_STRING(key), entries[i * 2 + 1]);
        }

        csPopTempRoot();
        vm.stackTop = entries;
        csVMPush(OBJ_VAL(object));
        VM_NEXT();
      }
