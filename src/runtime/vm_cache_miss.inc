/* vm_cache_miss.inc — what an inline cache misses.
 *
 * The fast paths are in the opcode bodies; these run when the cache does not
 * hit, and they have to agree with the fast paths exactly — a shape hit and a
 * shape miss must reach the same value, or a program means something different
 * the second time it runs.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
/* `__proto__` is a link, not a property. Recognising it by name is what makes
 * `{ __proto__: base }` and `o.__proto__ = base` work without the accessor
 * machinery JavaScript defines it with — and it is why an object cannot have
 * an ordinary property under that name, which the docs record. */
static bool isProtoKey(const ObjString *name) {
  return name->length == 9 && memcmp(name->chars, "__proto__", 9) == 0;
}

/* A method that is *not* an own property travels with the object it was read
 * from — a class method, or one found on the prototype chain.
 *
 * The rule is drawn there rather than at "every method" because an own method
 * lives in the object's shape, and so is served by the inline cache: binding
 * it would mean testing every property read in the VM for whether it happened
 * to produce a method. A class method and an inherited one can never be in the
 * receiver's own shape, so they always reach this slow path and binding them
 * costs nothing anyone else pays.
 *
 * Only a closure written *as* a method is bound. A plain function that happens
 * to be stored on an object is just a function, and giving it a receiver it
 * never asked for would be inventing meaning for its `this`. */
static Value bindIfMethod(Value receiver, Value found) {
  if (IS_CLOSURE(found) && AS_CLOSURE(found)->function->isMethod) {
    return OBJ_VAL(csBoundMethodNew(receiver, (Obj *)AS_CLOSURE(found)));
  }
  return found;
}

static bool propertyReadSlow(ObjString *name, PropertyCache *cache, Value receiver,
                             Value *out) {
  if (IS_OBJECT(receiver)) {
    ObjObject *object = AS_OBJECT(receiver);

    int slot;
    if (object->shape != NULL && csShapeLookup(object->shape, name, &slot)) {
      cache->shape = object->shape;
      cache->slot = slot;
      *out = object->as.slots.values[slot];
      return true;
    }

    /* Dictionary mode, an accessor, an inherited method, or a genuinely absent
     * property. Reading a missing property gives undefined, as in JavaScript —
     * checking for absence is too common to make it an error. */
    /* An own accessor's slot holds a stand-in rather than a value, so finding
     * it means the answer is further down, where accessors are looked for. */
    if (csObjectGet(object, name, out) && !csVMIsAccessorSlot(*out)) return true;

    /* `constructor` names whatever built the object: its class, or the
     * function `new` was applied to. Neither is an ordinary property — the
     * first is not stored at all and the second lives beside the shape — so
     * both are answered here rather than found by the walk below. */
    if (name->length == 11 && memcmp(name->chars, "constructor", 11) == 0) {
      for (ObjObject *at = object; at != NULL; at = at->prototype) {
        if (at->klass != NULL && !at->klass->isAccessorHolder) {
          *out = OBJ_VAL(at->klass);
          return true;
        }
        Value owner;
        if (csObjectGetPrivate(at, csStringCopy("#constructor", 12), &owner)) {
          *out = owner;
          return true;
        }
      }
      *out = UNDEFINED_VAL;
      return true;
    }

    /* `__proto__` reads the link itself rather than a property, which is the
     * only way to reach it without going through Object.getPrototypeOf. */
    if (isProtoKey(name)) {
      *out = object->prototype != NULL ? OBJ_VAL(object->prototype) : NULL_VAL;
      return true;
    }

    /* Everything else the object might answer with, at each link of the chain
     * in turn: a data property, an accessor, a method.
     *
     * An accessor lives on a class rather than in the shape — a synthetic one
     * per object for a literal's `get x()`, the real one for an instance — so
     * asking each link the same three questions is what makes an inherited
     * getter work at all. Doing it level by level is also what gives shadowing
     * the right answer: a nearer link's data property beats a further link's
     * getter, exactly as in JavaScript. */
    for (ObjObject *at = object; at != NULL; at = at->prototype) {
      /* The object's own data properties were checked above. */
      if (at != object && csObjectGet(at, name, out) &&
          !csVMIsAccessorSlot(*out)) {
        /* Bound to the object it was reached *through*, not the prototype it
         * was found on — that is what makes `this` mean the instance. */
        *out = bindIfMethod(receiver, *out);
        return true;
      }
      if (at->klass == NULL) continue;

      ObjClosure *getter = csClassFindGetter(at->klass, name);
      if (getter != NULL) {
        /* A getter is a call, so it runs a nested loop. It never enters the
         * shape, which is why the inline caches never see one. The receiver is
         * the object asked, not the one that turned out to define it. */
        csVMPush(receiver);
        return csVMCallCallback(OBJ_VAL(getter), 0, out);
      }

      ObjClosure *method = csClassFindMethod(at->klass, name);
      if (method != NULL) {
        /* Reading a method without calling it is the one case that has to
         * allocate, because the receiver has to travel with it. */
        *out = OBJ_VAL(csBoundMethodNew(receiver, (Obj *)method));
        return true;
      }
    }

    *out = UNDEFINED_VAL;
    return true;
  }

  /* A pattern's own properties. Intrinsic for the same reason `.length` is:
   * the object does not carry a table. `lastIndex` is the only writable one,
   * handled beside this in the write path. */
  if (IS_REGEX(receiver)) {
    ObjRegex *regex = AS_REGEX(receiver);
    if (name->length == 9 && memcmp(name->chars, "lastIndex", 9) == 0) {
      *out = NUMBER_VAL(regex->lastIndex);
      return true;
    }
    if (name->length == 6 && memcmp(name->chars, "source", 6) == 0) {
      *out = OBJ_VAL(regex->source);
      return true;
    }
    if (name->length == 5 && memcmp(name->chars, "flags", 5) == 0) {
      *out = OBJ_VAL(regex->flags);
      return true;
    }
    if (name->length == 6 && memcmp(name->chars, "global", 6) == 0) {
      *out = BOOL_VAL(regex->global);
      return true;
    }
  }

  /* `.size` on a Map or Set, like `.length` on an array: intrinsic rather than
   * stored, so the collection does not have to carry a property table. */
  if (IS_MAP(receiver) && name->length == 4 && memcmp(name->chars, "size", 4) == 0) {
    /* A weak collection has no size to report: the answer would depend on
     * when the collector last ran, which is the one thing a program must not
     * be able to find out. */
    if (AS_MAP(receiver)->isWeak) {
      csVMRuntimeError("a %s has no size: how many entries are left depends on "
                       "when the collector last ran",
                       AS_MAP(receiver)->isSet ? "WeakSet" : "WeakMap");
      return false;
    }
    *out = NUMBER_VAL(AS_MAP(receiver)->liveCount);
    return true;
  }

  /* Named properties on an array — only `exec` results have any. Checked
   * before `length`, which no caller can shadow anyway, and after nothing:
   * an ordinary array has no table and this is one NULL test. */
  if (IS_ARRAY(receiver)) {
    Value extra;
    if (csArrayGetExtra(AS_ARRAY(receiver), name, &extra)) {
      *out = extra;
      return true;
    }
  }

  /* `F.prototype` — the object everything `new F()` builds inherits from. It
   * is created on the first ask rather than with the closure, because most
   * functions are never constructed from. */
  if (IS_CLOSURE(receiver) && name->length == 9 &&
      memcmp(name->chars, "prototype", 9) == 0) {
    *out = OBJ_VAL(csClosurePrototype(AS_CLOSURE(receiver)));
    return true;
  }

  /* `length` is intrinsic rather than a stored property, so arrays and strings
   * answer it without carrying a table. A function's is how many arguments it
   * must be given, which is the same number its arity check uses. */
  if (name->length == 6 && memcmp(name->chars, "length", 6) == 0) {
    if (IS_ARRAY(receiver)) {
      *out = NUMBER_VAL(AS_ARRAY(receiver)->elements.count);
      return true;
    }
    if (IS_STRING(receiver)) {
      *out = NUMBER_VAL(AS_STRING(receiver)->length);
      return true;
    }
    if (IS_CLOSURE(receiver)) {
      *out = NUMBER_VAL(AS_CLOSURE(receiver)->function->arity);
      return true;
    }
    if (IS_NATIVE(receiver)) {
      int arity = AS_NATIVE(receiver)->arity;
      *out = NUMBER_VAL(arity < 0 ? 0 : arity);
      return true;
    }
  }

  /* A symbol's description, which is the only thing about one that is not its
   * identity. */
  if (IS_SYMBOL(receiver) && name->length == 11 &&
      memcmp(name->chars, "description", 11) == 0) {
    ObjString *described = AS_SYMBOL(receiver)->description;
    *out = described != NULL ? OBJ_VAL(described) : UNDEFINED_VAL;
    return true;
  }

  /* And `name`, which a stack trace and a program should agree about. */
  if (name->length == 4 && memcmp(name->chars, "name", 4) == 0) {
    if (IS_CLOSURE(receiver)) {
      ObjString *given = AS_CLOSURE(receiver)->function->name;
      *out = OBJ_VAL(given != NULL ? given : csStringCopy("", 0));
      return true;
    }
    if (IS_NATIVE(receiver)) {
      *out = OBJ_VAL(csStringCopy(AS_NATIVE(receiver)->name->chars,
                                  AS_NATIVE(receiver)->name->length));
      return true;
    }
    if (IS_CLASS(receiver)) {
      *out = OBJ_VAL(AS_CLASS(receiver)->name);
      return true;
    }
  }

  /* A callable may carry statics, which is how Number(x) and Number.isInteger
   * coexist. */
  if (IS_NATIVE(receiver) && AS_NATIVE(receiver)->statics != NULL) {
    Value statik;
    *out = csObjectGet(AS_NATIVE(receiver)->statics, name, &statik) ? statik
                                                                   : UNDEFINED_VAL;
    return true;
  }

  /* A class carries its statics, and inherits its parent's. */
  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value statik;
      if (csTableGet(&klass->statics, name, &statik)) {
        *out = statik;
        return true;
      }
      /* A static getter is run rather than handed back, and `this` inside it
       * is the class it was asked of. */
      Value getter;
      if (csTableGet(&klass->staticGetters, name, &getter)) {
        csVMPush(receiver);
        return csVMCallCallback(getter, 0, out);
      }
    }
    *out = UNDEFINED_VAL;
    return true;
  }

  csVMRuntimeError("cannot read property '%s' of %s", name->chars,
                   csValueTypeName(receiver));
  return false;
}

/* The part of a property write that is not a cache hit. */
static bool propertyWriteSlow(ObjString *name, PropertyCache *cache, Value receiver,
                              Value value) {
  /* `lastIndex` is writable, which is how a `g` pattern is rewound. */
  if (IS_REGEX(receiver) && name->length == 9 &&
      memcmp(name->chars, "lastIndex", 9) == 0) {
    if (!IS_NUMBER(value)) {
      csVMRuntimeError("lastIndex must be a number");
      return false;
    }
    AS_REGEX(receiver)->lastIndex = (int)AS_NUMBER(value);
    return true;
  }

  /* A class carries its statics, so assigning to one is how a counter kept on
   * the class is updated. It never enters a shape, so it never caches. */
  if (IS_CLASS(receiver)) {
    for (ObjClass *klass = AS_CLASS(receiver); klass != NULL;
         klass = klass->superclass) {
      Value setter;
      if (csTableGet(&klass->staticSetters, name, &setter)) {
        csVMPush(receiver);
        csVMPush(value);
        Value ignored;
        return csVMCallCallback(setter, 1, &ignored);
      }
      Value getter;
      if (csTableGet(&klass->staticGetters, name, &getter)) {
        csVMRuntimeError("'%s' has only a getter and cannot be assigned to",
                         name->chars);
        return false;
      }
    }
    csTableSet(&AS_CLASS(receiver)->statics, name, value);
    return true;
  }

  /* Replacing a function's prototype wholesale, which is how the pattern
   * `F.prototype = { m() {} }` is written. */
  if (IS_CLOSURE(receiver) && name->length == 9 &&
      memcmp(name->chars, "prototype", 9) == 0) {
    if (!IS_OBJECT(value)) {
      csVMRuntimeError("a prototype must be an object, got %s",
                       csValueTypeName(value));
      return false;
    }
    AS_CLOSURE(receiver)->prototype = AS_OBJECT(value);
    return true;
  }

  if (!IS_OBJECT(receiver)) {
    csVMRuntimeError("cannot set property '%s' of %s", name->chars,
                     csValueTypeName(receiver));
    return false;
  }
  if (AS_OBJECT(receiver)->frozen) {
    csVMRuntimeError("'%s.%s' cannot be changed: the object is frozen",
                     AS_OBJECT(receiver)->name->chars, name->chars);
    return false;
  }

  /* Assigning to `__proto__` re-links the object rather than storing anything,
   * which is what makes it the same link Object.setPrototypeOf sets. */
  if (isProtoKey(name)) {
    ObjObject *object = AS_OBJECT(receiver);
    if (IS_NULL(value) || IS_UNDEFINED(value)) {
      object->prototype = NULL;
      return true;
    }
    if (!IS_OBJECT(value)) {
      /* JavaScript ignores a non-object here rather than complaining, which
       * turns a mistake into a link that silently did not happen. */
      csVMRuntimeError("a prototype must be an object or null, got %s",
                       csValueTypeName(value));
      return false;
    }
    if (!csObjectSetPrototype(object, AS_OBJECT(value))) {
      csVMRuntimeError("that prototype is already in this object's chain, "
                       "which would make every lookup on it loop");
      return false;
    }
    return true;
  }

  /* A setter takes the write instead of the object storing it. Checked before
   * the store, so the property never enters the shape and the caches stay out
   * of it — and looked for up the whole chain, because a setter inherited from
   * a prototype is still the thing that owns this name. */
  for (ObjObject *at = AS_OBJECT(receiver); at != NULL; at = at->prototype) {
    if (at->klass == NULL) continue;
    ObjClosure *setter = csClassFindSetter(at->klass, name);
    if (setter != NULL) {
      csVMPush(receiver);
      csVMPush(value);
      Value ignored;
      return csVMCallCallback(OBJ_VAL(setter), 1, &ignored);
    }
    /* A getter with no setter is read-only, and silently dropping the write is
     * how a bug hides. */
    if (csClassFindGetter(at->klass, name) != NULL) {
      csVMRuntimeError("'%s' has only a getter and cannot be assigned to",
                       name->chars);
      return false;
    }
  }

  /* A property `Object.defineProperty` marked read-only refuses the write
   * rather than dropping it: a store that silently does nothing is how a bug
   * hides, and an ES module throws here too. The object left shape mode when
   * the property was defined, so the write fast path never reaches this
   * property and the check costs nothing anywhere else. */
  if ((csObjectAttributes(AS_OBJECT(receiver), name) & CS_PROP_WRITABLE) == 0) {
    csVMRuntimeError("'%s' is read-only and cannot be assigned to", name->chars);
    return false;
  }

  /* What the object had on the way in, so that an *add* can be cached as the
   * transition it is: this shape, that slot, and the shape it becomes. A site
   * that adds the same property to like-shaped objects — every `this.x = x` in
   * a constructor — then costs a compare and two stores instead of a lookup
   * and a table probe. */
  ObjObject *target = AS_OBJECT(receiver);
  Shape *before = target->shape;

  csObjectPut(target, name, value);

  int slot;
  if (target->shape != NULL && csShapeLookup(target->shape, name, &slot)) {
    cache->slot = slot;
    if (before == target->shape) {
      /* The property was already there: an overwrite, and the shape the site
       * expects is the one it also leaves behind. */
      cache->shape = target->shape;
      cache->added = NULL;
    } else {
      cache->shape = before;
      cache->added = target->shape;
    }
  }
  return true;
}
