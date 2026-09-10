/* vm_property.inc — accessors, and asking an object for something it may not have.
 *
 * An accessor's value is what its getter answers, which is a call — so reading
 * a property is not always a load, and every path that reads one has to go
 * through here rather than at the storage.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
bool csVMIsAccessorSlot(Value value) {
  return IS_OBJ(value) && AS_OBJ(value) == (Obj *)vm.accessorMarker;
}

unsigned csVMAccessorKind(ObjObject *object, ObjString *key) {
  if (object->klass == NULL) return 0;
  Value stored;
  if (!csObjectGet(object, key, &stored) || !csVMIsAccessorSlot(stored)) return 0;

  unsigned kind = 0;
  if (csClassFindGetter(object->klass, key) != NULL) kind |= CS_ACCESSOR_GET;
  if (csClassFindSetter(object->klass, key) != NULL) kind |= CS_ACCESSOR_SET;
  return kind;
}

bool csVMReadOwnProperty(ObjObject *object, ObjString *key, Value *out) {
  Value stored;
  if (!csObjectGet(object, key, &stored)) {
    *out = UNDEFINED_VAL;
    return true;
  }
  if (!csVMIsAccessorSlot(stored)) {
    *out = stored;
    return true;
  }

  /* An accessor with no getter reads as undefined, which is what a
   * write-only property is worth. */
  ObjClosure *getter = object->klass != NULL
                           ? csClassFindGetter(object->klass, key)
                           : NULL;
  if (getter == NULL) {
    *out = UNDEFINED_VAL;
    return true;
  }
  csVMPush(OBJ_VAL(object));
  return csVMCallCallback(OBJ_VAL(getter), 0, out);
}

/* An object's own `toString`, if it has one anywhere in its chain.
 *
 * JavaScript calls it whenever an object is converted to a string, and CScript
 * ignoring it was a silent wrong answer rather than a deliberate divergence:
 * the documented difference is only about what an object with *no* toString
 * renders as. Inspection is a separate question and stays as it was — Node's
 * console.log does not call toString either. */
static bool findUserToString(Value value, Value *method) {
  if (!IS_OBJECT(value)) return false;

  ObjString *name = csStringCopy("toString", 8);
  for (ObjObject *at = AS_OBJECT(value); at != NULL; at = at->prototype) {
    Value found;
    if (csObjectGet(at, name, &found) && IS_CLOSURE(found)) {
      *method = found;
      return true;
    }
    if (at->klass == NULL) continue;
    ObjClosure *declared = csClassFindMethod(at->klass, name);
    if (declared != NULL) {
      *method = OBJ_VAL(declared);
      return true;
    }
  }
  return false;
}

/* Text for `value` the way a string conversion wants it: through the object's
 * own `toString` when it has one, and through the built-in rendering
 * otherwise. The caller owns the returned buffer, as with csValueToCString. */
char *csVMValueToText(Value value, size_t *length) {
  Value method;
  if (!findUserToString(value, &method)) return csValueToCString(value, length);

  csVMPush(value); /* the receiver a method call reads out of slot 0 */
  Value produced;
  if (!csVMCallCallback(method, 0, &produced)) return NULL;

  /* Whatever it answered with is then converted the ordinary way, so a
   * toString that returns a number is not a second special case. */
  return csValueToCString(produced, length);
}

/* A module's exports as one frozen object.
 *
 * The keys are sorted, because that is what the specification says a
 * namespace's keys are — the exporting file's declaration order is
 * deliberately not observable through one. Returns false with an error already
 * reported when the module exports more names than one can hold. */
static bool buildNamespace(ObjModule *module, Value *out) {
  /* One namespace object per module: `import * as a` and `await import(…)` of
   * the same file give the same object in JavaScript, and rebuilding it would
   * quietly make them different. */
  if (module->namespaceView != NULL) {
    *out = OBJ_VAL(module->namespaceView);
    return true;
  }

  /* Collected first so the sort does not have to happen against a hash
   * table. */
  ObjString *names[CS_NAMESPACE_MAX];
  int count = 0;
  for (int i = 0; i < module->exports.capacity; i++) {
    Entry *entry = &module->exports.entries[i];
    if (entry->key == NULL) continue;
    if (count == CS_NAMESPACE_MAX) {
      csVMRuntimeError("'%s' exports more than %d names", module->path->chars,
                       CS_NAMESPACE_MAX);
      return false;
    }
    names[count++] = entry->key;
  }

  /* Insertion sort: export lists are short, and this keeps the comparison —
   * code-unit order, shorter first on a prefix — in one readable place. */
  for (int i = 1; i < count; i++) {
    ObjString *key = names[i];
    int j = i - 1;
    while (j >= 0) {
      int shorter = names[j]->length < key->length ? names[j]->length : key->length;
      int order = memcmp(names[j]->chars, key->chars, (size_t)shorter);
      if (order == 0) order = names[j]->length - key->length;
      if (order <= 0) break;
      names[j + 1] = names[j];
      j--;
    }
    names[j + 1] = key;
  }

  ObjObject *namespaceObject = csObjectNew(module->path->chars);
  /* Kept on the stack while the copies below, which allocate, run. */
  csVMPush(OBJ_VAL(namespaceObject));

  for (int i = 0; i < count; i++) {
    Value value;
    if (!csTableGet(&module->globals, names[i], &value)) continue;
    csObjectPut(namespaceObject, names[i], value);
  }

  /* A namespace is a view of another module, not somewhere to put things. */
  csObjectFreeze(namespaceObject);
  csVMPop();
  module->namespaceView = namespaceObject;
  *out = OBJ_VAL(namespaceObject);
  return true;
}
