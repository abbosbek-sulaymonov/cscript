/* vm_state.inc — the one VM, and the stack it runs on.
 *
 * There is a single global VM: it owns every heap object, the intern pool and
 * the globals map, which is also what makes it the collector's root set. So
 * setting it up and tearing it down is where the root set begins and ends.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */

void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.handlerCount = 0;
  vm.hasPendingException = false;
  vm.openUpvalues = NULL;
  vm.tempRootCount = 0;
}

/* The main program's execution state. A fiber for `await` gets its own arrays
 * on the heap; this one is static because it exists for the whole run. */
static Value mainStack[CS_STACK_MAX];
static CallFrame mainFrames[CS_FRAMES_MAX];
static ExceptionHandler mainHandlers[CS_HANDLERS_MAX];

void csVMInit(void) {
  vm.stack = mainStack;
  vm.stackCapacity = CS_STACK_MAX;
  vm.frames = mainFrames;
  vm.frameCapacity = CS_FRAMES_MAX;
  vm.handlers = mainHandlers;
  vm.handlerCapacity = CS_HANDLERS_MAX;
  resetStack();
  vm.objects = NULL;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;
  vm.sourceName = "<script>";

  csTableInit(&vm.builtins);
  csTableInit(&vm.builtinConsts);
  csTableInit(&vm.modules);
  vm.mainModule = NULL;
  vm.currentFiber = NULL;
  vm.fiberSuspended = false;
  vm.fiberYielded = false;
  vm.deferUncaught = false;
  vm.pendingCount = 0;
  csTableInit(&vm.strings);
  csTableInit(&vm.arrayMethods);
  csTableInit(&vm.stringMethods);
  csTableInit(&vm.promiseMethods);
  csTableInit(&vm.mapMethods);
  csTableInit(&vm.generatorMethods);
  csTableInit(&vm.numberMethods);
  csTableInit(&vm.functionMethods);
  csTableInit(&vm.dateMethods);
  csTableInit(&vm.weakMethods);
  csTableInit(&vm.symbolMethods);
  csTableInit(&vm.bigintMethods);
  vm.accessorMarker = NULL;
  vm.pendingNewTarget = UNDEFINED_VAL;
  csTableInit(&vm.symbolRegistry);
  csTableInit(&vm.symbolsByKey);
  vm.iteratorSymbol = NULL;
  vm.asyncIteratorSymbol = NULL;
  csTableInit(&vm.regexMethods);

  vm.microtasks = NULL;
  vm.microtaskCount = 0;
  vm.microtaskCapacity = 0;
  vm.microtaskHead = 0;
  vm.timerCount = 0;
  vm.nextTimerId = 1;
  vm.firingTimerId = -1;
  vm.firingCancelled = false;
  vm.timerSequence = 0;
  vm.rejected = NULL;
  vm.rejectedCount = 0;
  vm.rejectedCapacity = 0;

  /* Before any object exists: csObjectNew reads it, and the collector marks
   * it. */
  vm.emptyShape = NULL;
  vm.absentShape = NULL;
  vm.emptyShape = csShapeNewRoot();
  vm.absentShape = csShapeNewRoot();
  vm.accessorMarker = csObjectNew("<accessor>");

  csNativesInstall();

  /* Code with no file of its own — `-e`, the REPL, a string handed to
   * csInterpret — still needs a scope to live in. */
  vm.mainModule = csModuleNew(csStringCopy("<main>", 6));
}

void csVMMarkBuiltinConst(ObjString *name) {
  csTableSet(&vm.builtinConsts, name, BOOL_VAL(true));
}

void csVMFree(void) {
  CS_FREE_ARRAY(Microtask, vm.microtasks, vm.microtaskCapacity);
  CS_FREE_ARRAY(ObjPromise *, vm.rejected, vm.rejectedCapacity);
  csTableFree(&vm.promiseMethods);
  csTableFree(&vm.mapMethods);
  csTableFree(&vm.generatorMethods);
  csTableFree(&vm.numberMethods);
  csTableFree(&vm.functionMethods);
  csTableFree(&vm.dateMethods);
  csTableFree(&vm.weakMethods);
  csTableFree(&vm.symbolMethods);
  csTableFree(&vm.bigintMethods);
  csTableFree(&vm.symbolRegistry);
  csTableFree(&vm.symbolsByKey);
  csTableFree(&vm.regexMethods);
  csTableFree(&vm.builtins);
  csTableFree(&vm.builtinConsts);
  csTableFree(&vm.modules);
  csTableFree(&vm.arrayMethods);
  csTableFree(&vm.stringMethods);
  csTableFree(&vm.strings);
  csFreeAllObjects();
}

void csVMPush(Value value) {
  if (vm.stackTop - vm.stack >= vm.stackCapacity) {
    fprintf(stderr, "cscript: value stack overflow\n");
    resetStack();
    return;
  }
  *vm.stackTop++ = value;
}

Value csVMPop(void) { return *(--vm.stackTop); }

static Value peekStack(int distance) { return vm.stackTop[-1 - distance]; }

Value csVMPeek(int distance) { return peekStack(distance); }

/* True when a double holds an exact integer small enough to move into an
 * int64_t without loss. 2^53 is the largest such value for a double, and it is
 * far inside int64_t's range, so the conversion below cannot overflow. */
static inline bool isExactInteger(double value) {
  return value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
         value == (double)(int64_t)value;
}

/* How a file is named in a stack trace: relative to where the program was
 * started when it sits underneath, absolute otherwise. Traces cross module
 * boundaries now, so each frame names its own file rather than sharing one. */
static const char *frameSourceName(const CallFrame *frame) {
  ObjModule *module = frame->closure->function->module;
  if (module == NULL || module == vm.mainModule) return vm.sourceName;
  return csModuleDisplayPath(module->path->chars);
}
