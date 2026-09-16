/* object_gc.c — what the collector does with each object type.
 *
 * Two walks, and they have to agree: blackening follows every reference an
 * object holds, and freeing releases everything it owns. A type added to one
 * and not the other either leaks or collects something still reachable, which
 * is why they sit next to each other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/regex.h"
#include "cscript/shape.h"
#include "cscript/table.h"
#include "cscript/vm.h"

#include "runtime/object_internal.h"

void csObjectBlacken(Obj *object) {
  switch (object->type) {
    case OBJ_STRING: break; /* no outgoing references */

    case OBJ_NATIVE: {
      ObjNative *native = (ObjNative *)object;
      csMarkObject((Obj *)native->name);
      csMarkObject((Obj *)native->statics);
      break;
    }

    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      csMarkObject((Obj *)instance->name);
      csMarkObject((Obj *)instance->klass);
      csMarkObject((Obj *)instance->prototype);
      if (instance->shape != NULL) {
        /* The shape is the authority on how many slots hold a value. Anything
         * beyond slotCount is capacity the object has not grown into yet. */
        csMarkObject((Obj *)instance->shape);
        for (int i = 0; i < instance->shape->slotCount; i++) {
          csMarkValue(instance->as.slots.values[i]);
        }
      } else {
        csTableMark(&instance->as.dictionary.table);
        for (int i = 0; i < instance->as.dictionary.count; i++) {
          csMarkObject((Obj *)instance->as.dictionary.keys[i]);
        }
      }
      if (instance->attributes != NULL) csTableMark(instance->attributes);
      if (instance->privates != NULL) {
        csTableMark(instance->privates);
        /* A property keyed by a symbol keeps that symbol alive, which is what
         * lets Object.getOwnPropertySymbols still name it. The private table
         * holds only the filing string, so the symbol is looked up from it. */
        for (int i = 0; i < instance->privates->capacity; i++) {
          ObjString *key = instance->privates->entries[i].key;
          if (key == NULL) continue;
          Value symbol;
          if (csTableGet(&vm.symbolsByKey, key, &symbol)) csMarkValue(symbol);
        }
      }
      break;
    }

    case OBJ_SHAPE: csShapeBlacken((Shape *)object); break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csMarkObject((Obj *)klass->name);
      csMarkObject((Obj *)klass->superclass);
      csMarkObject((Obj *)klass->initializer);
      csMarkObject((Obj *)klass->fieldInit);
      csTableMark(&klass->methods);
      csTableMark(&klass->statics);
      csTableMark(&klass->getters);
      csTableMark(&klass->staticGetters);
      csTableMark(&klass->staticSetters);
      csTableMark(&klass->setters);
      break;
    }

    case OBJ_BOUND_METHOD: {
      ObjBoundMethod *bound = (ObjBoundMethod *)object;
      csMarkValue(bound->receiver);
      csMarkObject(bound->method);
      csMarkObject((Obj *)bound->presets);
      break;
    }

    case OBJ_REGEX: {
      ObjRegex *regex = (ObjRegex *)object;
      csMarkObject((Obj *)regex->source);
      csMarkObject((Obj *)regex->flags);
      break;
    }

    case OBJ_MAP: {
      ObjMap *map = (ObjMap *)object;
      /* A weak map marks neither: its keys are what make it weak, and its
       * values are marked afterwards, but only for the keys that turned out to
       * be alive. See csMarkEphemerons. */
      if (map->isWeak) break;

      /* Only the live entries: a tombstone's key and value were cleared when
       * it was deleted, so there is nothing there to keep alive. */
      for (int i = 0; i < map->count; i++) {
        if (!map->entries[i].present) continue;
        csMarkValue(map->entries[i].key);
        csMarkValue(map->entries[i].value);
      }
      break;
    }

    case OBJ_FIBER: {
      ObjFiber *fiber = (ObjFiber *)object;
      /* Only up to stackTop: everything above is the slots it has not grown
       * into yet, and marking those would walk stale values. */
      for (Value *slot = fiber->stack; slot < fiber->stackTop; slot++) {
        csMarkValue(*slot);
      }
      for (int i = 0; i < fiber->frameCount; i++) {
        csMarkObject((Obj *)fiber->frames[i].closure);
        csMarkValue(fiber->frames[i].newTarget);
      }
      for (ObjUpvalue *upvalue = fiber->openUpvalues; upvalue != NULL; upvalue = upvalue->next) {
        csMarkObject((Obj *)upvalue);
      }
      csMarkObject((Obj *)fiber->promise);
      csMarkObject((Obj *)fiber->generator);
      /* The fiber this one interrupted. Marking it is what keeps the stack it
       * is holding — and everything on it — from being swept while it runs. */
      csMarkObject((Obj *)fiber->caller);
      break;
    }

    case OBJ_DATE: break; /* a number and nothing else */

    case OBJ_BIGINT: break; /* limbs, and nothing that can be collected */

    case OBJ_SYMBOL: {
      ObjSymbol *symbol = (ObjSymbol *)object;
      csMarkObject((Obj *)symbol->description);
      csMarkObject((Obj *)symbol->key);
      break;
    }

    case OBJ_GENERATOR: {
      ObjGenerator *generator = (ObjGenerator *)object;
      csMarkObject((Obj *)generator->fiber);
      csMarkObject((Obj *)generator->pendingResult);
      csMarkValue(generator->yielded);
      break;
    }

    case OBJ_PROMISE: {
      ObjPromise *promise = (ObjPromise *)object;
      csMarkValue(promise->value);
      for (int i = 0; i < promise->reactionCount; i++) {
        csMarkValue(promise->reactions[i].onFulfilled);
        csMarkValue(promise->reactions[i].onRejected);
        csMarkObject((Obj *)promise->reactions[i].result);
        csMarkObject((Obj *)promise->reactions[i].combineState);
        csMarkObject((Obj *)promise->reactions[i].fiber);
      }
      break;
    }

    case OBJ_MODULE: {
      ObjModule *module = (ObjModule *)object;
      csMarkObject((Obj *)module->namespaceView);
      csMarkObject((Obj *)module->path);
      csMarkObject((Obj *)module->body);
      csTableMark(&module->globals);
      csTableMark(&module->globalConsts);
      csTableMark(&module->exports);
      break;
    }

    case OBJ_FUNCTION: {
      /* A function owns its constant pool, so every literal in its body is
       * live for as long as the function is. */
      ObjFunction *function = (ObjFunction *)object;
      csMarkObject((Obj *)function->name);
      csMarkObject((Obj *)function->module);
      for (int i = 0; i < function->chunk.constants.count; i++) {
        csMarkValue(function->chunk.constants.values[i]);
      }
      break;
    }

    case OBJ_UPVALUE: {
      /* `closed` holds the value once the variable has left the stack. While
       * the upvalue is still open it is empty, and the stack root covers it. */
      ObjUpvalue *upvalue = (ObjUpvalue *)object;
      csMarkValue(upvalue->closed);
      /* And the fiber that stack belongs to, which may be reachable from
       * nothing else — see ObjUpvalue.home. */
      csMarkObject((Obj *)upvalue->home);
      break;
    }

    case OBJ_CLOSURE: {
      ObjClosure *closure = (ObjClosure *)object;
      csMarkObject((Obj *)closure->function);
      csMarkObject((Obj *)closure->prototype);
      for (int i = 0; i < closure->upvalueCount; i++) {
        csMarkObject((Obj *)closure->upvalues[i]);
      }
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      if (array->extras != NULL) csTableMark(array->extras);
      for (int i = 0; i < array->elements.count; i++) {
        csMarkValue(array->elements.values[i]);
      }
      break;
    }
  }
}

void csObjectFree(Obj *object) {
  switch (object->type) {
    case OBJ_STRING: {
      ObjString *string = (ObjString *)object;
      csReallocate(object, sizeof(ObjString) + (size_t)string->length + 1, 0);
      break;
    }
    case OBJ_NATIVE: CS_FREE(ObjNative, object); break;
    case OBJ_OBJECT: {
      ObjObject *instance = (ObjObject *)object;
      if (instance->shape != NULL) {
        CS_FREE_ARRAY(Value, instance->as.slots.values, instance->as.slots.capacity);
      } else {
        csTableFree(&instance->as.dictionary.table);
        CS_FREE_ARRAY(ObjString *, instance->as.dictionary.keys, instance->as.dictionary.capacity);
      }
      if (instance->privates != NULL) {
        csTableFree(instance->privates);
        CS_FREE(Table, instance->privates);
      }
      if (instance->attributes != NULL) {
        csTableFree(instance->attributes);
        CS_FREE(Table, instance->attributes);
      }
      CS_FREE(ObjObject, object);
      break;
    }

    case OBJ_SHAPE: csShapeFree((Shape *)object); break;

    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *)object;
      csTableFree(&klass->methods);
      csTableFree(&klass->statics);
      csTableFree(&klass->getters);
      csTableFree(&klass->staticGetters);
      csTableFree(&klass->staticSetters);
      csTableFree(&klass->setters);
      CS_FREE(ObjClass, object);
      break;
    }

    case OBJ_BOUND_METHOD:
      /* The receiver and the method both belong to whoever else holds them. */
      CS_FREE(ObjBoundMethod, object);
      break;

    case OBJ_REGEX: {
      ObjRegex *regex = (ObjRegex *)object;
      csRegexFree(regex->program);
      CS_FREE(ObjRegex, object);
      break;
    }

    case OBJ_MAP: {
      ObjMap *map = (ObjMap *)object;
      CS_FREE_ARRAY(MapEntry, map->entries, map->capacity);
      CS_FREE_ARRAY(int, map->index, map->indexCapacity);
      CS_FREE(ObjMap, object);
      break;
    }

    case OBJ_FIBER: {
      ObjFiber *fiber = (ObjFiber *)object;
      CS_FREE_ARRAY(Value, fiber->stack, fiber->stackCapacity);
      CS_FREE_ARRAY(CallFrame, fiber->frames, CS_FIBER_FRAMES);
      CS_FREE_ARRAY(ExceptionHandler, fiber->handlers, CS_FIBER_HANDLERS);
      CS_FREE(ObjFiber, object);
      break;
    }

    case OBJ_DATE: CS_FREE(ObjDate, object); break;

    case OBJ_BIGINT:
      csBigFree(&((ObjBigInt *)object)->value);
      CS_FREE(ObjBigInt, object);
      break;

    case OBJ_SYMBOL: CS_FREE(ObjSymbol, object); break;

    case OBJ_GENERATOR: {
      /* The fiber is an object of its own and is swept on its own. */
      CS_FREE(ObjGenerator, object);
      break;
    }

    case OBJ_PROMISE: {
      ObjPromise *promise = (ObjPromise *)object;
      CS_FREE_ARRAY(Reaction, promise->reactions, promise->reactionCapacity);
      CS_FREE(ObjPromise, object);
      break;
    }

    case OBJ_MODULE: {
      ObjModule *module = (ObjModule *)object;
      csTableFree(&module->globals);
      csTableFree(&module->globalConsts);
      csTableFree(&module->exports);
      /* The types this file declared, which outlived the arena that parsed it
       * precisely so that an importer could read them. */
      free(module->types);
      for (int i = 0; i < module->exportTypeCount; i++) free(module->exportTypes[i].name);
      free(module->exportTypes);
      CS_FREE(ObjModule, object);
      break;
    }

    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *)object;
      free(function->paramTypes);
      free(function->observedParams);
      csChunkFree(&function->chunk);
      CS_FREE(ObjFunction, object);
      break;
    }

    case OBJ_UPVALUE:
      /* The captured value belongs to whoever else still references it. */
      CS_FREE(ObjUpvalue, object);
      break;

    case OBJ_CLOSURE: {
      /* The function is shared between closures, so only the array goes. */
      ObjClosure *closure = (ObjClosure *)object;
      CS_FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCount);
      CS_FREE(ObjClosure, object);
      break;
    }

    case OBJ_ARRAY: {
      ObjArray *array = (ObjArray *)object;
      csValueArrayFree(&array->elements);
      if (array->extras != NULL) {
        csTableFree(array->extras);
        CS_FREE(Table, array->extras);
      }
      CS_FREE(ObjArray, object);
      break;
    }
  }
}
