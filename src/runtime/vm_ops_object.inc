/* vm_ops_object.inc — spread, private members, and deleting a property.
 *
 * A private field never takes a shape slot, which is what makes it invisible
 * to `Object.keys`, `JSON.stringify` and a subscript — so it needs its own
 * pair of opcodes rather than a name the ordinary ones would accept.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */
      VM_CASE(OP_SPREAD_MARK) {
        /* A Map or Set spreads to what iterating it yields, which is the same
         * conversion `for...of` makes — they have to agree. */
        if (IS_MAP(peekStack(0)) && AS_MAP(peekStack(0))->isWeak) {
          csVMRuntimeError("a %s cannot be spread: what is left in it depends on "
                           "when the collector last ran",
                           AS_MAP(peekStack(0))->isSet ? "WeakSet" : "WeakMap");
          return CS_RUNTIME_ERROR;
        }
        if (IS_MAP(peekStack(0))) {
          ObjArray *items = csMapToArray(AS_MAP(peekStack(0)));
          csVMPop();
          csVMPush(OBJ_VAL(items));
        }

        /* An object that offers `Symbol.iterator` spreads to what it yields,
         * which is the same conversion `for...of` makes — the two have to
         * agree about what a value contains. */
        {
          Value iteratorMethod;
          if (findIteratorMethod(peekStack(0), &iteratorMethod)) {
            Value iterator;
            if (!callWithReceiver(iteratorMethod, peekStack(0), NULL, 0, &iterator)) {
              return CS_RUNTIME_ERROR;
            }
            csPushTempRoot(AS_OBJ(iterator));

            ObjArray *drained = csArrayNew();
            csPushTempRoot((Obj *)drained);
            for (;;) {
              Value produced;
              bool done;
              if (!iteratorStep(iterator, &produced, &done)) {
                csPopTempRoot();
                csPopTempRoot();
                return CS_RUNTIME_ERROR;
              }
              if (done) break;
              csValueArrayWrite(&drained->elements, produced);
            }
            csPopTempRoot();
            csPopTempRoot();

            csVMPop();
            csVMPush(OBJ_VAL(drained));
            frame = &vm.frames[vm.frameCount - 1];
          }
        }

        /* A generator spreads to everything it has left. Spreading is eager by
         * definition — the array has to be complete before it exists — so
         * running the body to its end here is not a shortcut, it is what the
         * operation means. An endless generator does not terminate, which is
         * equally true in JavaScript. */
        if (IS_GENERATOR(peekStack(0))) {
          ObjGenerator *generator = AS_GENERATOR(peekStack(0));
          ObjArray *drained = csArrayNew();
          csPushTempRoot((Obj *)drained);
          for (;;) {
            Value produced;
            bool done;
            if (!csGeneratorNext(generator, UNDEFINED_VAL, &produced, &done)) {
              csPopTempRoot();
              return CS_RUNTIME_ERROR;
            }
            if (done) break;
            csValueArrayWrite(&drained->elements, produced);
          }
          csPopTempRoot();
          csVMPop();
          csVMPush(OBJ_VAL(drained));
        }

        Value value = peekStack(0);
        if (!IS_ARRAY(value) && !IS_STRING(value)) {
          csVMRuntimeError("only arrays and strings can be spread, got %s",
                           csValueTypeName(value));
          return CS_RUNTIME_ERROR;
        }

        /* Wrap in a marked array so the builder below can tell a spread
         * element from a plain array element. A string spreads to characters. */
        ObjArray *marker = csArrayNew();
        marker->isSpreadMarker = true;
        csPushTempRoot((Obj *)marker);

        if (IS_ARRAY(value)) {
          ObjArray *source = AS_ARRAY(value);
          for (int i = 0; i < source->elements.count; i++) {
            csValueArrayWrite(&marker->elements, source->elements.values[i]);
          }
        } else {
          ObjString *source = AS_STRING(value);
          for (int i = 0; i < source->length; i++) {
            ObjString *piece = csStringCopy(source->chars + i, 1);
            csPushTempRoot((Obj *)piece);
            csValueArrayWrite(&marker->elements, OBJ_VAL(piece));
            csPopTempRoot();
          }
        }

        csPopTempRoot();
        csVMPop();
        csVMPush(OBJ_VAL(marker));
        VM_NEXT();
      }

      VM_CASE(OP_ARRAY_SPREAD) {
        int count = READ_BYTE();
        ObjArray *array = csArrayNew();
        csPushTempRoot((Obj *)array);

        Value *elements = vm.stackTop - count;
        for (int i = 0; i < count; i++) {
          if (IS_ARRAY(elements[i]) && AS_ARRAY(elements[i])->isSpreadMarker) {
            ObjArray *spread = AS_ARRAY(elements[i]);
            for (int j = 0; j < spread->elements.count; j++) {
              csValueArrayWrite(&array->elements, spread->elements.values[j]);
            }
          } else {
            csValueArrayWrite(&array->elements, elements[i]);
          }
        }

        csPopTempRoot();
        vm.stackTop = elements;
        csVMPush(OBJ_VAL(array));
        VM_NEXT();
      }

      VM_CASE(OP_GET_PRIVATE) {
        ObjString *name = READ_STRING();
        Value target = csVMPop();
        Value value;
        /* `static #n` belongs to the class, where the statics already live —
         * and a name with a hash in it is no more reachable there. */
        if (IS_CLASS(target)) {
          csVMPush(csTableGet(&AS_CLASS(target)->statics, name, &value)
                       ? value
                       : UNDEFINED_VAL);
          VM_NEXT();
        }
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot read %s of %s", name->chars,
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(csObjectGetPrivate(AS_OBJECT(target), name, &value)
                     ? value
                     : UNDEFINED_VAL);
        VM_NEXT();
      }

      VM_CASE(OP_SET_PRIVATE) {
        ObjString *name = READ_STRING();
        Value value = peekStack(0);
        Value target = peekStack(1);
        if (IS_CLASS(target)) {
          csTableSet(&AS_CLASS(target)->statics, name, value);
          vm.stackTop -= 2;
          csVMPush(value);
          VM_NEXT();
        }
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot write %s of %s", name->chars,
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        csObjectPutPrivate(AS_OBJECT(target), name, value);
        vm.stackTop -= 2;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_DELETE_PROPERTY) {
        ObjString *name = READ_STRING();
        Value target = csVMPop();
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot delete a property of %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        if (AS_OBJECT(target)->frozen) {
          csVMRuntimeError("'%s' is frozen, so its properties cannot be deleted",
                           AS_OBJECT(target)->name->chars);
          return CS_RUNTIME_ERROR;
        }
        if ((csObjectAttributes(AS_OBJECT(target), name) &
             CS_PROP_CONFIGURABLE) == 0) {
          csVMRuntimeError("'%s' is not configurable and cannot be deleted",
                           name->chars);
          return CS_RUNTIME_ERROR;
        }
        /* JavaScript answers true when the property was not there either —
         * false is reserved for one it refused to remove, and a frozen
         * built-in is reported above rather than answered. */
        csObjectDelete(AS_OBJECT(target), name);
        csVMPush(BOOL_VAL(true));
        VM_NEXT();
      }

      VM_CASE(OP_DELETE_INDEX) {
        Value index = peekStack(0);
        Value target = peekStack(1);
        if (!IS_OBJECT(target)) {
          csVMRuntimeError("cannot delete a property of %s",
                           csValueTypeName(target));
          return CS_RUNTIME_ERROR;
        }
        if (AS_OBJECT(target)->frozen) {
          csVMRuntimeError("'%s' is frozen, so its properties cannot be deleted",
                           AS_OBJECT(target)->name->chars);
          return CS_RUNTIME_ERROR;
        }

        if (IS_SYMBOL(index)) {
          csObjectDeletePrivate(AS_OBJECT(target), AS_SYMBOL(index)->key);
          vm.stackTop -= 2;
          csVMPush(BOOL_VAL(true));
          VM_NEXT();
        }

        ObjString *key = objectKeyFor(index);
        if (key == NULL) {
          csVMRuntimeError("out of memory converting an object key");
          return CS_RUNTIME_ERROR;
        }
        csObjectDelete(AS_OBJECT(target), key);
        vm.stackTop -= 2;
        csVMPush(BOOL_VAL(true));
        VM_NEXT();
      }

      VM_CASE(OP_INVOKE_INDEX) {
        /* `o[key](args...)`, with the stack holding [receiver][key][args…].
         *
         * A computed callee is not always a method: it may be an element of an
         * array of functions, or a symbol-keyed method, neither of which the
         * by-name dispatch below would find. Those are read first, exactly as
         * `o[key]` reads them; everything else goes to the same dispatch a
         * dotted call takes, which is what makes `"abc"["toUpperCase"]()`
         * reach the built-in method tables. */
        int argCount = READ_BYTE();
        Value keyValue = vm.stackTop[-argCount - 1];
        Value receiver = vm.stackTop[-argCount - 2];

        Value found;
        bool haveValue = false;
        if (IS_SYMBOL(keyValue) && IS_OBJECT(receiver)) {
          haveValue = csObjectGetPrivate(AS_OBJECT(receiver),
                                         AS_SYMBOL(keyValue)->key, &found);
        } else if (IS_ARRAY(receiver) && IS_NUMBER(keyValue)) {
          ObjArray *array = AS_ARRAY(receiver);
          int index = (int)AS_NUMBER(keyValue);
          haveValue = index >= 0 && index < array->elements.count;
          if (haveValue) found = array->elements.values[index];
        }

        ObjString *name = NULL;
        if (!haveValue) {
          name = IS_SYMBOL(keyValue) ? AS_SYMBOL(keyValue)->key
                                     : objectKeyFor(keyValue);
          if (name == NULL) {
            csVMRuntimeError("out of memory converting a method name");
            return CS_RUNTIME_ERROR;
          }
        }
        if (name != NULL) csPushTempRoot((Obj *)name);

        /* Collapsing the key out leaves exactly the layout OP_INVOKE builds by
         * hand, so both paths below see the stack they expect. */
        memmove(&vm.stackTop[-argCount - 1], &vm.stackTop[-argCount],
                sizeof(Value) * (size_t)argCount);
        vm.stackTop--;

        bool ok;
        if (!haveValue) {
          ok = invokeMethod(receiver, name, argCount);
        } else if (IS_CLOSURE(found) && wantsReceiver(AS_CLOSURE(found))) {
          /* callMethod reads the receiver out of the callee slot, so that slot
           * is left exactly as it is. */
          ok = callMethod(AS_CLOSURE(found), argCount);
        } else {
          vm.stackTop[-argCount - 1] = found;
          ok = callValue(found, argCount);
        }
        if (name != NULL) csPopTempRoot();
        if (!ok) return CS_RUNTIME_ERROR;

        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }
