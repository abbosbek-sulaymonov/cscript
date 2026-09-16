/* vm_ops_value.inc — constants, the stack, variables and properties.
 *
 * The most-executed opcodes in the set, and the ones the superinstruction work
 * fused: an operand-stack push is three instructions of real work and the
 * dispatch around it used to cost more than that.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */
  VM_BEGIN
      VM_CASE(OP_CONSTANT)  csVMPush(READ_CONSTANT()); VM_NEXT();
      VM_CASE(OP_NULL)      csVMPush(NULL_VAL); VM_NEXT();
      VM_CASE(OP_UNDEFINED) csVMPush(UNDEFINED_VAL); VM_NEXT();
      VM_CASE(OP_TRUE)      csVMPush(BOOL_VAL(true)); VM_NEXT();
      VM_CASE(OP_FALSE)     csVMPush(BOOL_VAL(false)); VM_NEXT();

      VM_CASE(OP_POP) csVMPop(); VM_NEXT();
      VM_CASE(OP_POP_N) vm.stackTop -= READ_BYTE(); VM_NEXT();
      VM_CASE(OP_DUP) csVMPush(peekStack(0)); VM_NEXT();

      /* Both exist for reading a target that is also being written: `o.x += 1`
       * needs `o` on the stack twice, and `xs[i] += 1` needs `xs` and `i`. */
      VM_CASE(OP_DUP2) {
        Value under = peekStack(1);
        Value top = peekStack(0);
        csVMPush(under);
        csVMPush(top);
        VM_NEXT();
      }
      VM_CASE(OP_POP_UNDER) {
        Value top = csVMPop();
        csVMPop();
        csVMPush(top);
        VM_NEXT();
      }

      VM_CASE(OP_DEFINE_GLOBAL) {
        ObjString *name = READ_STRING();
        csTableSet(&frameModule(frame)->globals, name, peekStack(0));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_DEFINE_CONST) {
        ObjString *name = READ_STRING();
        ObjModule *module = frameModule(frame);
        csTableSet(&module->globals, name, peekStack(0));
        csTableSet(&module->globalConsts, name, BOOL_VAL(true));
        csVMPop();
        VM_NEXT();
      }

      VM_CASE(OP_GET_GLOBAL) {
        ObjString *name = READ_STRING();
        GlobalCache *cache = READ_GLOBAL_CACHE();

        /* Globals are declared once and never deleted, so after this site has
         * run once the entry it wants is fixed. The version check is what
         * makes holding a pointer into the table safe: only a rehash or a
         * delete can move an entry, and both bump it. */
        if (cache->filled && cache->version == cache->table->version) {
          csVMPush(cache->entry->value);
          VM_NEXT();
        }

        ObjModule *module = frameModule(frame);
        Entry *entry = csTableFindEntry(&module->globals, name);
        if (entry == NULL) {
          /* JavaScript would return undefined for a bare read in sloppy mode.
           * Reading a name that was never declared is a typo, not an intent. */
          csVMRuntimeError("'%s' is not defined", name->chars);
          return CS_RUNTIME_ERROR;
        }
        cache->table = &module->globals;
        cache->entry = entry;
        cache->version = module->globals.version;
        cache->filled = true;
        csVMPush(entry->value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_GLOBAL) {
        ObjString *name = READ_STRING();
        GlobalCache *cache = READ_GLOBAL_CACHE();
        if (cache->filled && cache->version == cache->table->version) {
          cache->entry->value = peekStack(0);
          VM_NEXT();
        }
        if (!assignGlobal(frameModule(frame), name, cache, peekStack(0))) {
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

      VM_CASE(OP_GET_LOCAL_LOCAL) {
        Value *slots = frame->slots;
        uint8_t a = READ_BYTE();
        uint8_t b = READ_BYTE();
        csVMPush(slots[a]);
        csVMPush(slots[b]);
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
        GlobalCache *cache = READ_GLOBAL_CACHE();
        if (cache->filled && cache->version == cache->table->version) {
          cache->entry->value = csVMPop();
          VM_NEXT();
        }
        if (!assignGlobal(frameModule(frame), name, cache, peekStack(0))) {
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

      VM_CASE(OP_GET_LOCAL_PROPERTY) {
        /* `this.x` and `local.x`: the receiver is read straight out of its slot
         * rather than pushed and popped. Its own handler rather than a jump
         * into OP_GET_PROPERTY, because the dispatch table's indirect branch is
         * predicted per opcode and sharing one body loses that. */
        Value receiver = frame->slots[READ_BYTE()];
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();

        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          csVMPush(AS_OBJECT(receiver)->as.slots.values[cache->slot]);
          VM_NEXT();
        }

        Value value;
        if (!propertyReadSlow(name, cache, receiver, &value)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_GET_PROPERTY) {
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value receiver = peekStack(0);

        /* The point of the whole shape machinery: when this site sees the
         * layout it saw last time — which is what happens when every object
         * reaching it came from the same literal — reading a property is a
         * pointer compare and an indexed load. */
        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape) {
          vm.stackTop[-1] = AS_OBJECT(receiver)->as.slots.values[cache->slot];
          VM_NEXT();
        }

        Value value;
        if (!propertyReadSlow(name, cache, receiver, &value)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        vm.stackTop[-1] = value;
        VM_NEXT();
      }

      VM_CASE(OP_SET_PROPERTY) {
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value value = peekStack(0);
        Value receiver = peekStack(1);

        /* Overwriting a property that already exists cannot change the shape
         * and cannot allocate. It also cannot be a built-in namespace member:
         * that case errors out below, so such a site never fills its cache.
         *
         * Adding one is the same two instructions plus adopting the layout the
         * site saw last time — as long as the storage already has room, which
         * is the only part that could allocate. */
        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape && csVMStoreCached(AS_OBJECT(receiver), cache, value)) {
          vm.stackTop -= 2;
          csVMPush(value);
          VM_NEXT();
        }

        if (!propertyWriteSlow(name, cache, receiver, value)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        /* Leave the assigned value: assignment is an expression. */
        vm.stackTop -= 2;
        csVMPush(value);
        VM_NEXT();
      }

      VM_CASE(OP_SET_PROPERTY_POP) {
        /* The same store, in statement position: the value nothing reads is
         * never written back. */
        ObjString *name = READ_STRING();
        PropertyCache *cache = READ_PROPERTY_CACHE();
        Value value = peekStack(0);
        Value receiver = peekStack(1);

        if (IS_OBJECT(receiver) && AS_OBJECT(receiver)->shape == cache->shape && csVMStoreCached(AS_OBJECT(receiver), cache, value)) {
          vm.stackTop -= 2;
          VM_NEXT();
        }

        if (!propertyWriteSlow(name, cache, receiver, value)) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        vm.stackTop -= 2;
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

        if (IS_STRING(target)) {
          /* Indexing a string yields a one-character string, as in JavaScript,
           * and out of range is undefined rather than an error. */
          if (!IS_NUMBER(index)) {
            csVMRuntimeError("string index must be a number, got %s",
                             csValueTypeName(index));
            return CS_RUNTIME_ERROR;
          }
          ObjString *string = AS_STRING(target);
          int slot = (int)AS_NUMBER(index);
          vm.stackTop -= 2;
          if (slot < 0 || slot >= string->length) {
            csVMPush(UNDEFINED_VAL);
          } else {
            csVMPush(OBJ_VAL(csStringCopy(string->chars + slot, 1)));
          }
          VM_NEXT();
        }

        if (IS_OBJECT(target)) {
          /* A symbol names a property that nothing walking the object by name
           * can reach, so it is kept beside the shape rather than in it. */
          if (IS_SYMBOL(index)) {
            Value found;
            bool has = csObjectGetPrivate(AS_OBJECT(target), AS_SYMBOL(index)->key,
                                          &found);
            vm.stackTop -= 2;
            csVMPush(has ? found : UNDEFINED_VAL);
            VM_NEXT();
          }

          /* `o[1]` and `o["1"]` name the same property, so the key converts
           * rather than being rejected — which is also what `{ 1: x }` had to
           * store under for the two to meet. */
          ObjString *key = objectKeyFor(index);
          if (key == NULL) {
            csVMRuntimeError("out of memory converting an object key");
            return CS_RUNTIME_ERROR;
          }
          Value value;
          bool found = csObjectGetInherited(AS_OBJECT(target), key, &value);
          vm.stackTop -= 2;
          csVMPush(found ? value : UNDEFINED_VAL);
          VM_NEXT();
        }

        csVMRuntimeError("cannot index %s", csValueTypeName(target));
        return CS_RUNTIME_ERROR;
      }
