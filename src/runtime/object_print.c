/* object_print.c — how each object prints, which is not how it converts.
 *
 * `console.log` of an object shows its structure, quotes nested strings and
 * says `[Circular]` rather than recursing for ever; `"" + object` is a
 * conversion and lives in value.c. Node distinguishes them and so does this.
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

static void printFunctionName(const ObjFunction *function) {
  if (function->name == NULL) {
    printf("[Function: <script>]");
  } else {
    printf("[Function: %s]", function->name->chars);
  }
}

/* A value shown inside a container is quoted if it is a string, so that
 * `[ '1' ]` and `[ 1 ]` are distinguishable. Three places want that. */
static void printNested(Value value) {
  if (IS_STRING(value)) {
    printf("'%s'", AS_CSTRING(value));
  } else {
    csValuePrint(value);
  }
}

void csObjectPrint(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value));
      break;
    case OBJ_NATIVE:
      printf("[Function: %s]", AS_NATIVE(value)->name->chars);
      break;
    case OBJ_OBJECT:
      printf("[Object: %s]", AS_OBJECT(value)->name->chars);
      break;
    case OBJ_CLASS: {
      ObjClass *klass = AS_CLASS(value);
      if (klass->superclass != NULL) {
        printf("[class %s extends %s]", klass->name->chars,
               klass->superclass->name->chars);
      } else {
        printf("[class %s]", klass->name->chars);
      }
      break;
    }
    case OBJ_BOUND_METHOD: {
      Obj *method = AS_BOUND_METHOD(value)->method;
      if (method->type == OBJ_NATIVE) {
        printf("[Function: %s]", ((ObjNative *)method)->name->chars);
      } else {
        printFunctionName(((ObjClosure *)method)->function);
      }
      break;
    }
    case OBJ_MODULE:
      printf("[Module: %s]", AS_MODULE(value)->path->chars);
      break;
    case OBJ_REGEX:
      printf("/%s/%s", AS_REGEX(value)->source->chars, AS_REGEX(value)->flags->chars);
      break;

    case OBJ_MAP: {
      /* `Map(2) { 'a' => 1 }` and `Set(2) { 1, 2 }`, as Node prints them. */
      ObjMap *map = AS_MAP(value);
      if (map->isWeak) {
        /* Not even the count: it would say when the collector last ran. */
        printf("%s { <items unknown> }", map->isSet ? "WeakSet" : "WeakMap");
        break;
      }
      printf("%s(%d)", map->isSet ? "Set" : "Map", map->liveCount);
      if (map->liveCount == 0) {
        printf(" {}");
        break;
      }
      printf(" { ");
      bool first = true;
      for (int i = 0; i < map->count; i++) {
        if (!map->entries[i].present) continue;
        if (!first) printf(", ");
        first = false;
        printNested(map->entries[i].key);
        if (!map->isSet) {
          printf(" => ");
          printNested(map->entries[i].value);
        }
      }
      printf(" }");
      break;
    }

    case OBJ_FIBER:
      printf("[internal]");
      break;
    case OBJ_GENERATOR:
      printf("Object [Generator] {}");
      break;
    case OBJ_BIGINT: {
      char *text = csBigToText(&AS_BIGINT(value)->value, 10);
      printf("%sn", text != NULL ? text : "0");
      free(text);
      break;
    }
    case OBJ_SYMBOL: {
      ObjSymbol *symbol = AS_SYMBOL(value);
      printf("Symbol(%s)",
             symbol->description != NULL ? symbol->description->chars : "");
      break;
    }
    case OBJ_DATE: {
      char text[64];
      if (csDateToISO(AS_DATE(value)->ms, text, sizeof text)) {
        printf("%s", text);
      } else {
        printf("Invalid Date");
      }
      break;
    }
    case OBJ_PROMISE: {
      ObjPromise *promise = AS_PROMISE(value);
      if (promise->state == PROMISE_PENDING) {
        printf("Promise { <pending> }");
      } else {
        printf("Promise { ");
        if (promise->state == PROMISE_REJECTED) printf("<rejected> ");
        /* Quoted, the way a value nested inside a container is printed. */
        if (IS_STRING(promise->value)) {
          printf("'%s'", AS_CSTRING(promise->value));
        } else {
          csValuePrint(promise->value);
        }
        printf(" }");
      }
      break;
    }
    case OBJ_FUNCTION:
      printFunctionName((ObjFunction *)AS_OBJ(value));
      break;
    case OBJ_CLOSURE:
      printFunctionName(AS_CLOSURE(value)->function);
      break;
    case OBJ_UPVALUE:
    case OBJ_SHAPE:
      /* Never reachable from user code; only the collector sees these. */
      printf("[internal]");
      break;

    case OBJ_ARRAY: {
      ObjArray *array = AS_ARRAY(value);
      printf("[ ");
      for (int i = 0; i < array->elements.count; i++) {
        if (i > 0) printf(", ");
        /* Strings are quoted inside a container, the way console.log does it,
         * so `[ "1" ]` and `[ 1 ]` are distinguishable. */
        if (IS_STRING(array->elements.values[i])) {
          printf("'%s'", AS_CSTRING(array->elements.values[i]));
        } else {
          csValuePrint(array->elements.values[i]);
        }
      }
      printf(" ]");
      break;
    }
  }
}
