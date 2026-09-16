/* vm_throw.inc — where a throw goes.
 *
 * A runtime error and a `throw` are the same mechanism: both unwind to the
 * innermost handler belonging to this loop. What separates them is only
 * whether a program can catch the result.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
/* Unwinds to the innermost handler, if that handler belongs to this loop.
 *
 * `baseFrame` is where this interpreter loop started. A handler installed below
 * it was installed by an outer loop, whose C frame is still on the stack under
 * this one — so the throw has to travel out through the return path rather than
 * jump there directly. */
ThrowResult performThrow(Value thrown, int baseFrame, CallFrame **frame) {
  if (vm.handlerCount == 0) return THROW_UNCAUGHT;

  ExceptionHandler *handler = &vm.handlers[vm.handlerCount - 1];
  /* This loop is entered with frameCount == baseFrame + 1, so a handler
   * recorded at baseFrame or below was installed before it started and belongs
   * to an outer loop. Jumping there would resume outer code with this loop's C
   * frame — and the native that started it — still on the stack. */
  if (handler->frameCount <= baseFrame) {
    vm.pendingException = thrown;
    vm.hasPendingException = true;
    return THROW_PROPAGATE;
  }

  vm.handlerCount--;

  /* Discard every frame the throw escaped, closing anything their locals were
   * captured into before the slots are reused. */
  while (vm.frameCount > handler->frameCount) {
    vm.frameCount--;
    closeUpvalues(vm.frames[vm.frameCount].slots);
  }
  closeUpvalues(handler->stackTop);

  vm.stackTop = handler->stackTop;
  *frame = &vm.frames[vm.frameCount - 1];
  (*frame)->ip = handler->ip;

  /* The catch block expects the thrown value where its binding lives. */
  csVMPush(thrown);
  return THROW_HANDLED;
}

static void reportUncaught(Value thrown) {
  size_t length = 0;
  char *text = csValueInspect(thrown, &length);
  csVMRuntimeError("uncaught %s", text != NULL ? text : "value");
  free(text);
}

/* Runs the field initialisers of every class from `klass` up to but not
 * including `stopAt`, base-most first.
 *
 * Only classes that declare no constructor of their own get here: a class with
 * a constructor has its fields compiled into it, at the top of the body for a
 * base class and immediately after `super(...)` for a derived one. That is
 * where JavaScript runs them, and running them anywhere else is observable —
 * it changes the order of `Object.keys`. */
bool runFieldInitializers(ObjClass *klass, ObjClass *stopAt, Value instance) {
  ObjClass *chain[CS_FRAMES_MAX];
  int depth = 0;
  for (ObjClass *current = klass; current != stopAt; current = current->superclass) {
    if (depth == CS_FRAMES_MAX) {
      csVMRuntimeError("class hierarchy is too deep (limit %d)", CS_FRAMES_MAX);
      return false;
    }
    chain[depth++] = current;
  }

  for (int i = depth - 1; i >= 0; i--) {
    if (chain[i]->fieldInit == NULL) continue;
    /* Pushed where the callee would go, so the initialiser's slot 0 — its
     * `this` — is the instance. */
    csVMPush(instance);
    Value ignored;
    if (!csVMCallCallback(OBJ_VAL(chain[i]->fieldInit), 0, &ignored)) return false;
  }
  return true;
}

/* The nearest class at or above `klass` that declares a constructor. A class
 * without one is built by its parent's, which is what makes
 * `class Dog extends Animal {}` still accept the arguments Animal declares. */
static ObjClass *findConstructorOwner(ObjClass *klass) {
  for (ObjClass *current = klass; current != NULL; current = current->superclass) {
    if (current->initializer != NULL) return current;
  }
  return NULL;
}

ObjClass *csVMConstructorOwner(ObjClass *klass) { return findConstructorOwner(klass); }

/* The nearest constructor at or above `klass`, or NULL. */
ObjClosure *findConstructor(ObjClass *klass) {
  ObjClass *owner = findConstructorOwner(klass);
  return owner != NULL ? owner->initializer : NULL;
}


/* The scope a frame's global reads and writes resolve in. Every function in a
 * file carries the module it was compiled in. */
static ObjModule *frameModule(const CallFrame *frame) {
  return frame->closure->function->module;
}

/* The uncached path for assigning to a global: check that the name exists and
 * is not a constant, then record where it lives so the site never has to ask
 * again.
 *
 * Filling the cache is what licenses the fast path to skip the constant check.
 * A name's constness is fixed when it is declared, and a site cannot run
 * before the declaration it refers to — reaching one would have errored here
 * with "is not defined" and left the cache empty. */
static bool assignGlobal(ObjModule *module, ObjString *name, GlobalCache *cache,
                         Value value) {
  if (csTableGet(&module->globalConsts, name, NULL)) {
    csVMRuntimeError("'%s' is a constant and cannot be reassigned", name->chars);
    return false;
  }

  /* Assigning to a name that does not exist would silently create a global in
   * JavaScript; here it is the typo it almost always is. */
  Entry *entry = csTableFindEntry(&module->globals, name);
  if (entry == NULL) {
    csVMRuntimeError("'%s' is not defined", name->chars);
    return false;
  }

  entry->value = value;
  cache->table = &module->globals;
  cache->entry = entry;
  cache->version = module->globals.version;
  cache->filled = true;
  return true;
}

/* What happens to a throw that escapes every handler.
 *
 * Inside an async function it is not an error yet: the call already handed its
 * caller a promise, so the escaping value becomes that promise's rejection and
 * whoever awaits it decides what to do. Anywhere else there is no one left to
 * tell, and it is reported. */
static InterpretResult uncaught(Value thrown) {
  if (vm.currentFiber != NULL || vm.deferUncaught) {
    vm.pendingException = thrown;
    vm.hasPendingException = true;
    return CS_RUNTIME_ERROR;
  }
  reportUncaught(thrown);
  return CS_RUNTIME_ERROR;
}

/* The part of a property read that is not a cache hit. Kept out of line so
 * each of the opcodes that reads a property can have its own tight handler:
 * sharing one body between them costs more than the instruction it saves,
 * because the dispatch table's indirect jump is predicted per opcode. */
