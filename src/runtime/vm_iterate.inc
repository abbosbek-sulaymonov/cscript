/* vm_iterate.inc — object keys, and what `for...of` pulls from.
 *
 * Three shapes: an index, a generator, and an object that offers
 * `Symbol.iterator`. Which one runs is decided per iteration by looking at the
 * iterable, so the index path is unchanged by the others existing.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
/* The string a value becomes when used as an object key.
 *
 * JavaScript converts anything: `o[1]` and `o["1"]` are the same property.
 * Returns NULL only when the conversion could not allocate, in which case the
 * caller reports it. The result is unrooted, so a caller that allocates again
 * before storing it must root it first. */
static ObjString *objectKeyFor(Value key) {
  if (IS_STRING(key)) return AS_STRING(key);
  /* A symbol is filed under a name no source can write, which is what keeps a
   * symbol-keyed property out of everything that walks an object by name —
   * exactly where JavaScript puts one. */
  if (IS_SYMBOL(key)) return AS_SYMBOL(key)->key;

  size_t length = 0;
  char *text = csValueToCString(key, &length);
  if (text == NULL) return NULL;
  ObjString *converted = csStringCopy(text, (int)length);
  free(text);
  return converted;
}

/* Every symbol whose property this object carries.
 *
 * A symbol-keyed property is filed under a string, so the string has to be
 * mapped back — which is what `vm.symbolsByKey` is for. Nothing else needs it,
 * and it is weak in the sense that matters: it holds only symbols that some
 * object is already keyed by. */
void csVMCollectSymbolKeys(ObjObject *object, ObjArray *into) {
  if (object->privates == NULL) return;

  for (int i = 0; i < object->privates->capacity; i++) {
    ObjString *key = object->privates->entries[i].key;
    if (key == NULL) continue;

    Value symbol;
    if (!csTableGet(&vm.symbolsByKey, key, &symbol)) continue;
    csValueArrayWrite(&into->elements, symbol);
  }
}

/* The function an object offers as its `Symbol.iterator`, or NULL.
 *
 * Looked for where a symbol-keyed property lives — beside the shape — and then
 * on the class, so both `{ [Symbol.iterator]() {} }` and a class that declares
 * the method work. */
static bool findIteratorMethod(Value target, Value *out) {
  if (!IS_OBJECT(target) || vm.iteratorSymbol == NULL) return false;
  ObjObject *object = AS_OBJECT(target);

  if (csObjectGetPrivate(object, vm.iteratorSymbol->key, out)) return true;
  if (object->klass != NULL) {
    ObjClosure *method = csClassFindMethod(object->klass, vm.iteratorSymbol->key);
    if (method != NULL) {
      *out = OBJ_VAL(method);
      return true;
    }
  }
  return false;
}

/* The same, for `Symbol.asyncIterator`. `for await` asks for this one first
 * and falls back to the sync protocol, which is what JavaScript does. */
static bool findAsyncIteratorMethod(Value target, Value *out) {
  if (!IS_OBJECT(target) || vm.asyncIteratorSymbol == NULL) return false;
  ObjObject *object = AS_OBJECT(target);

  if (csObjectGetPrivate(object, vm.asyncIteratorSymbol->key, out)) return true;
  if (object->klass != NULL) {
    ObjClosure *method = csClassFindMethod(object->klass, vm.asyncIteratorSymbol->key);
    if (method != NULL) {
      *out = OBJ_VAL(method);
      return true;
    }
  }
  return false;
}

/* Calls `callee` with `receiver` in slot 0, for the places the VM itself has
 * to invoke a user method: the iterator protocol, so far. */
static bool callWithReceiver(Value callee, Value receiver, Value *args, int argCount,
                             Value *out) {
  if (!IS_CLOSURE(callee)) return csVMCallAdapted(callee, args, argCount, out);

  ObjBoundMethod *bound = csBoundMethodNew(receiver, AS_OBJ(callee));
  csPushTempRoot((Obj *)bound);
  bool ok = csVMCallAdapted(OBJ_VAL(bound), args, argCount, out);
  csPopTempRoot();
  return ok;
}

/* One step of a user-defined iterator: `it.next()`, unpacked.
 *
 * A generator is the common case and is already an iterator, so it is pulled
 * directly rather than through a property lookup and a call. */
static bool iteratorStep(Value iterator, Value *value, bool *done) {
  if (IS_GENERATOR(iterator)) {
    return csGeneratorNext(AS_GENERATOR(iterator), UNDEFINED_VAL, value, done);
  }

  Value next;
  if (!IS_OBJECT(iterator) ||
      (!csObjectGet(AS_OBJECT(iterator), csStringCopy("next", 4), &next) &&
       (AS_OBJECT(iterator)->klass == NULL ||
        (next = OBJ_VAL(csClassFindMethod(AS_OBJECT(iterator)->klass,
                                          csStringCopy("next", 4))),
         !IS_CLOSURE(next))))) {
    csVMRuntimeError("an iterator must have a 'next' method");
    return false;
  }

  Value record;
  if (!callWithReceiver(next, iterator, NULL, 0, &record)) return false;
  if (!IS_OBJECT(record)) {
    csVMRuntimeError("'next' must answer with an object, got %s",
                     csValueTypeName(record));
    return false;
  }

  Value flag;
  *done = csObjectGet(AS_OBJECT(record), csStringCopy("done", 4), &flag) &&
          csValueIsTruthy(flag);
  if (!csObjectGet(AS_OBJECT(record), csStringCopy("value", 5), value)) {
    *value = UNDEFINED_VAL;
  }
  return true;
}

/* Whether `invokeMethod` would find something to call.
 *
 * Deliberately the same walk in the same order, because an answer that
 * disagreed with the call that follows it would turn `o.m?.()` into either a
 * skipped call or a runtime error. */
static bool receiverHasMethod(Value receiver, ObjString *name) {
  Value found;

  if (IS_OBJECT(receiver)) {
    ObjObject *instance = AS_OBJECT(receiver);
    Value ignored;
    if (name->length > 0 && name->chars[0] == '#' &&
        csObjectGetPrivate(instance, name, &ignored)) {
      return true;
    }
    if (csObjectGetInherited(instance, name, &found)) return true;
    return instance->klass != NULL &&
           csClassFindMethod(instance->klass, name) != NULL;
  }

  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      if (csTableGet(&klass->statics, name, &found)) return true;
      if (csTableGet(&klass->staticGetters, name, &found)) return true;
    }
    return false;
  }

  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    return csObjectGet(AS_NATIVE(receiver)->statics, name, &found);
  }

  Table *methods = methodTableFor(receiver);
  return methods != NULL && csTableGet(methods, name, &found);
}
