#include <stdio.h>
#include <stdlib.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

void *csReallocate(void *pointer, size_t oldSize, size_t newSize) {
  vm.bytesAllocated += newSize - oldSize;

  if (newSize > oldSize) {
#ifdef CS_DEBUG_STRESS_GC
    csCollectGarbage();
#else
    if (vm.bytesAllocated > vm.nextGC) csCollectGarbage();
#endif
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }

  void *result = realloc(pointer, newSize);
  if (result == NULL) {
    fprintf(stderr, "cscript: out of memory (requested %zu bytes)\n", newSize);
    exit(70);
  }
  return result;
}

void csPushTempRoot(Obj *object) {
  if (vm.tempRootCount >= CS_TEMP_ROOTS_MAX) {
    fprintf(stderr, "cscript: temporary root stack overflow\n");
    exit(70);
  }
  vm.tempRoots[vm.tempRootCount++] = object;
}

void csPopTempRoot(void) {
  if (vm.tempRootCount > 0) vm.tempRootCount--;
}

void csMarkObject(Obj *object) {
  if (object == NULL || object->isMarked) return;

#ifdef CS_DEBUG_LOG_GC
  printf("%p mark ", (void *)object);
  csValuePrint(OBJ_VAL(object));
  printf("\n");
#endif

  object->isMarked = true;

  /* The worklist uses raw realloc on purpose: routing it through csReallocate
   * would let a collection trigger another collection. */
  if (vm.grayCapacity < vm.grayCount + 1) {
    vm.grayCapacity = CS_GROW_CAPACITY(vm.grayCapacity);
    vm.grayStack = (Obj **)realloc(vm.grayStack, sizeof(Obj *) * (size_t)vm.grayCapacity);
    if (vm.grayStack == NULL) {
      fprintf(stderr, "cscript: out of memory growing the GC worklist\n");
      exit(70);
    }
  }
  vm.grayStack[vm.grayCount++] = object;
}

void csMarkValue(Value value) {
  if (IS_OBJ(value)) csMarkObject(AS_OBJ(value));
}

/* Marks everything reachable from `object`. The per-type walk lives in
 * object.c, next to the definitions of the types it has to know about. */
static void blackenObject(Obj *object) {
#ifdef CS_DEBUG_LOG_GC
  printf("%p blacken ", (void *)object);
  csValuePrint(OBJ_VAL(object));
  printf("\n");
#endif

  csObjectBlacken(object);
}

static void markRoots(void) {
  for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
    csMarkValue(*slot);
  }

  /* Every active call keeps its closure alive, and a closure keeps its function
   * and therefore its whole constant pool alive — OP_CONSTANT can push any
   * literal at any point, so all of them are live for the length of the call. */
  for (int i = 0; i < vm.frameCount; i++) {
    csMarkObject((Obj *)vm.frames[i].closure);
  }

  /* Upvalues still pointing into the stack. Their `closed` field is empty while
   * they are open, but the object itself must survive. */
  for (ObjUpvalue *upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    csMarkObject((Obj *)upvalue);
  }

  /* An exception travelling out of a nested interpreter loop is referenced by
   * nothing else while it is in flight. */
  if (vm.hasPendingException) csMarkValue(vm.pendingException);

  csMarkObject((Obj *)vm.emptyShape);
  csMarkObject((Obj *)vm.absentShape);
  /* Anything queued is reachable from nowhere else: a microtask holds the only
   * reference to its handler and its result promise until it runs. */
  for (int i = vm.microtaskHead; i < vm.microtaskCount; i++) {
    csMarkValue(vm.microtasks[i].callback);
    csMarkValue(vm.microtasks[i].argument);
    csMarkObject((Obj *)vm.microtasks[i].result);
    csMarkObject((Obj *)vm.microtasks[i].combineState);
    csMarkObject((Obj *)vm.microtasks[i].fiber);
  }
  for (int i = 0; i < vm.timerCount; i++) {
    csMarkValue(vm.timers[i].callback);
  }
  for (int i = 0; i < vm.rejectedCount; i++) {
    csMarkObject((Obj *)vm.rejected[i]);
  }

  csPromiseMarkRoots();
  csTableMark(&vm.promiseMethods);
  csTableMark(&vm.builtins);
  csTableMark(&vm.builtinConsts);
  csTableMark(&vm.modules);
  csMarkObject((Obj *)vm.mainModule);
  csMarkObject((Obj *)vm.currentFiber);
  /* Built-in methods live only in these tables, so nothing else keeps them
   * alive between calls. */
  csTableMark(&vm.arrayMethods);
  csTableMark(&vm.stringMethods);
  for (int i = 0; i < vm.tempRootCount; i++) {
    csMarkObject(vm.tempRoots[i]);
  }
  csCompilerMarkRoots();
}

static void traceReferences(void) {
  while (vm.grayCount > 0) {
    blackenObject(vm.grayStack[--vm.grayCount]);
  }
}

/* Weak references, resolved once marking is final and before anything is
 * freed. Both kinds exist for the same reason: caching a layout, or recording
 * a route to one, must not be what keeps it alive.
 *
 * Walking every live object to find them costs one extra pass. A dedicated
 * list of shapes and chunks would avoid it, but at the price of a second
 * structure to keep in step with the sweep list — and this pass is the same
 * order as the sweep that immediately follows it. */
static void pruneWeakReferences(void) {
  for (Obj *object = vm.objects; object != NULL; object = object->next) {
    if (!object->isMarked) continue;
    if (object->type == OBJ_SHAPE) {
      csShapePruneTransitions((Shape *)object);
    } else if (object->type == OBJ_FUNCTION) {
      csChunkPruneCaches(&((ObjFunction *)object)->chunk);
    }
  }
}

static void sweep(void) {
  Obj *previous = NULL;
  Obj *object = vm.objects;

  while (object != NULL) {
    if (object->isMarked) {
      object->isMarked = false; /* reset for the next cycle */
      previous = object;
      object = object->next;
      continue;
    }

    Obj *unreached = object;
    object = object->next;
    if (previous == NULL) {
      vm.objects = object;
    } else {
      previous->next = object;
    }
    csObjectFree(unreached);
  }
}

void csCollectGarbage(void) {
#ifdef CS_DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = vm.bytesAllocated;
#endif

  markRoots();
  traceReferences();
  /* The intern pool holds weak references: drop entries whose string died,
   * before sweeping frees the memory those keys point at. */
  csTableRemoveWhite(&vm.strings);
  pruneWeakReferences();
  sweep();

  vm.nextGC = vm.bytesAllocated * CS_GC_HEAP_GROW_FACTOR;
  if (vm.nextGC < 1024 * 1024) vm.nextGC = 1024 * 1024;

#ifdef CS_DEBUG_LOG_GC
  printf("-- gc end: collected %zu bytes (%zu -> %zu), next at %zu\n",
         before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
#endif
}

void csFreeAllObjects(void) {
  Obj *object = vm.objects;
  while (object != NULL) {
    Obj *next = object->next;
    csObjectFree(object);
    object = next;
  }
  vm.objects = NULL;

  free(vm.grayStack);
  vm.grayStack = NULL;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
}
