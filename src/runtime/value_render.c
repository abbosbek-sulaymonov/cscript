/* value_render.c — turning a Value into text, in the two ways that differ.
 *
 * `console.log(o)` shows an object's structure, quotes nested strings and says
 * `[Circular]` rather than recursing for ever; `"" + o` is a conversion and
 * gives `[object Object]`. Node distinguishes them and so does this, which is
 * why one builder serves both with a flag rather than two builders drifting
 * apart.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/table.h"
#include "cscript/value.h"
#include "cscript/vm.h"

#include "runtime/value_internal.h"

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} StringBuilder;

static bool sbAppend(StringBuilder *builder, const char *text, size_t length) {
  if (builder->length + length + 1 > builder->capacity) {
    size_t capacity = builder->capacity < 32 ? 32 : builder->capacity;
    while (capacity < builder->length + length + 1) capacity *= 2;
    char *grown = (char *)realloc(builder->data, capacity);
    if (grown == NULL) return false;
    builder->data = grown;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, text, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool sbAppendValue(StringBuilder *builder, Value value, bool quoteStrings);

/* `Map(2) { 'a' => 1 }` and `Set(2) { 1, 2 }`, as Node prints them. */
static bool sbAppendMap(StringBuilder *builder, ObjMap *map) {
  /* A weak one shows nothing, not even how many: the count would say when the
   * collector last ran, and that is not something a program may find out. */
  if (map->isWeak) {
    const char *opaque = map->isSet ? "WeakSet { <items unknown> }"
                                    : "WeakMap { <items unknown> }";
    return sbAppend(builder, opaque, strlen(opaque));
  }

  char header[32];
  int length = snprintf(header, sizeof header, "%s(%d)",
                        map->isSet ? "Set" : "Map", map->liveCount);
  if (!sbAppend(builder, header, (size_t)length)) return false;
  if (map->liveCount == 0) return sbAppend(builder, " {}", 3);
  if (!sbAppend(builder, " { ", 3)) return false;

  bool first = true;
  for (int i = 0; i < map->count; i++) {
    if (!map->entries[i].present) continue;
    if (!first && !sbAppend(builder, ", ", 2)) return false;
    first = false;
    if (!sbAppendValue(builder, map->entries[i].key, true)) return false;
    if (map->isSet) continue;
    if (!sbAppend(builder, " => ", 4)) return false;
    if (!sbAppendValue(builder, map->entries[i].value, true)) return false;
  }
  return sbAppend(builder, " }", 2);
}

/* `Promise { 1 }`, `Promise { <pending> }`, `Promise { <rejected> 'why' }` —
 * the same shapes Node prints, so a program that logs one still matches. */
static bool sbAppendPromise(StringBuilder *builder, ObjPromise *promise) {
  if (promise->state == PROMISE_PENDING) {
    return sbAppend(builder, "Promise { <pending> }", 21);
  }
  if (!sbAppend(builder, "Promise { ", 10)) return false;
  if (promise->state == PROMISE_REJECTED &&
      !sbAppend(builder, "<rejected> ", 11)) {
    return false;
  }
  return sbAppendValue(builder, promise->value, true) && sbAppend(builder, " }", 2);
}

static bool sbAppendArray(StringBuilder *builder, ObjArray *array) {
  if (!sbAppend(builder, "[ ", array->elements.count > 0 ? 2 : 1)) return false;
  for (int i = 0; i < array->elements.count; i++) {
    if (i > 0 && !sbAppend(builder, ", ", 2)) return false;
    /* Strings are quoted inside a container so `[ '1' ]` and `[ 1 ]` differ. */
    if (!sbAppendValue(builder, array->elements.values[i], true)) return false;
  }
  return sbAppend(builder, array->elements.count > 0 ? " ]" : "]",
                  array->elements.count > 0 ? 2 : 1);
}

static bool sbAppendObject(StringBuilder *builder, ObjObject *object) {
  /* An instance prints under the name of whatever built it — `Dog { name: 'Rex' }`
   * — which is what makes one distinguishable from a plain literal at a
   * glance. A class instance is named by its class; one built by `new` on a
   * function is named by the function, which is how Node labels it too. A
   * plain literal carries the name "Object" and prints without a label, the
   * same way Node leaves that one out. */
  ObjString *label = NULL;
  if (object->klass != NULL && !object->klass->isAccessorHolder) {
    label = object->klass->name;
  } else if (object->builtByConstructor && object->name != NULL) {
    label = object->name;
  }
  if (label != NULL) {
    if (!sbAppend(builder, label->chars, (size_t)label->length)) return false;
    if (!sbAppend(builder, " ", 1)) return false;
  }
  if (!sbAppend(builder, "{", 1)) return false;

  bool first = true;
  for (int i = 0; i < csObjectCount(object); i++) {
    ObjString *key = csObjectKeyAt(object, i);
    if (!csObjectIsEnumerable(object, key)) continue;
    Value value = csObjectValueAt(object, i);

    if (!sbAppend(builder, first ? " " : ", ", first ? 1 : 2)) return false;
    first = false;
    if (!sbAppend(builder, key->chars, (size_t)key->length)) return false;
    if (!sbAppend(builder, ": ", 2)) return false;

    /* An accessor is named rather than run. Inspecting an object must not have
     * side effects, and a getter is a call — Node prints `[Getter]` for the
     * same reason. */
    if (csVMIsAccessorSlot(value)) {
      unsigned kind = csVMAccessorKind(object, key);
      const char *shown = kind == (CS_ACCESSOR_GET | CS_ACCESSOR_SET)
                              ? "[Getter/Setter]"
                          : kind == CS_ACCESSOR_SET ? "[Setter]"
                                                    : "[Getter]";
      if (!sbAppend(builder, shown, strlen(shown))) return false;
      continue;
    }
    if (!sbAppendValue(builder, value, true)) return false;
  }

  return sbAppend(builder, first ? "}" : " }", first ? 1 : 2);
}

/* Every value that reports as a function, whatever shape it has underneath. */
#define IS_CALLABLE(v)                                                    \
  (IS_NATIVE(v) || IS_FUNCTION(v) || IS_CLOSURE(v) || IS_BOUND_METHOD(v) || \
   IS_CLASS(v))

/* `[Function: name]`, `[Function (anonymous)]` or `[class Name extends Base]`,
 * matching what Node prints — which is the only reason to prefer any of these
 * over a bare placeholder.
 *
 * Built on the heap rather than in a fixed buffer because a name can be any
 * length. */
static char *renderCallable(Value value, size_t *lengthOut) {
  ObjString *name = NULL;
  ObjClass *klass = NULL;

  if (IS_NATIVE(value)) {
    name = AS_NATIVE(value)->name;
  } else if (IS_FUNCTION(value)) {
    name = AS_FUNCTION(value)->name;
  } else if (IS_CLOSURE(value)) {
    name = AS_CLOSURE(value)->function->name;
  } else if (IS_BOUND_METHOD(value)) {
    Obj *method = AS_BOUND_METHOD(value)->method;
    name = method->type == OBJ_NATIVE ? ((ObjNative *)method)->name
                                      : ((ObjClosure *)method)->function->name;
  } else {
    klass = AS_CLASS(value);
    name = klass->name;
  }

  StringBuilder builder = {NULL, 0, 0};
  bool ok;
  if (klass != NULL) {
    ok = sbAppend(&builder, "[class ", 7) &&
         sbAppend(&builder, name->chars, (size_t)name->length);
    if (ok && klass->superclass != NULL) {
      ok = sbAppend(&builder, " extends ", 9) &&
           sbAppend(&builder, klass->superclass->name->chars,
                    (size_t)klass->superclass->name->length);
    }
    ok = ok && sbAppend(&builder, "]", 1);
  } else if (name == NULL) {
    ok = sbAppend(&builder, "[Function (anonymous)]", 22);
  } else {
    ok = sbAppend(&builder, "[Function: ", 11) &&
         sbAppend(&builder, name->chars, (size_t)name->length) &&
         sbAppend(&builder, "]", 1);
  }

  if (!ok) {
    free(builder.data);
    return NULL;
  }
  if (lengthOut != NULL) *lengthOut = builder.length;
  return builder.data;
}

static bool sbAppendValue(StringBuilder *builder, Value value, bool quoteStrings) {
  /* `quoteStrings` marks the inspect path — inside a container, or a
   * console.log argument — where -0 is shown with its sign. */
  if (quoteStrings && IS_NUMBER(value)) {
    char buffer[32];
    int length = csValueFormatNumberEx(buffer, sizeof(buffer), AS_NUMBER(value), true);
    return sbAppend(builder, buffer, (size_t)length);
  }

  if (IS_BIGINT(value)) {
    /* Shown with the `n` it is written with. This is the inspect path only —
     * `String(1n)` and `"" + 1n` both give "1", as they do in JavaScript, and
     * they go through csValueToCString instead. */
    char *rendered = csBigToText(&AS_BIGINT(value)->value, 10);
    if (rendered == NULL) return false;
    bool ok = sbAppend(builder, rendered, strlen(rendered)) &&
              sbAppend(builder, "n", 1);
    free(rendered);
    return ok;
  }

  if (IS_OBJ(value)) {
    if (IS_ARRAY(value)) return sbAppendArray(builder, AS_ARRAY(value));
    if (IS_OBJECT(value)) return sbAppendObject(builder, AS_OBJECT(value));
    if (IS_PROMISE(value)) return sbAppendPromise(builder, AS_PROMISE(value));
    if (IS_MAP(value)) return sbAppendMap(builder, AS_MAP(value));
    if (IS_REGEX(value)) {
      /* `/a\d+/gi` — the source as written, which is why the object keeps it. */
      ObjRegex *regex = AS_REGEX(value);
      return sbAppend(builder, "/", 1) &&
             sbAppend(builder, regex->source->chars, (size_t)regex->source->length) &&
             sbAppend(builder, "/", 1) &&
             sbAppend(builder, regex->flags->chars, (size_t)regex->flags->length);
    }
    if (IS_STRING(value) && quoteStrings) {
      ObjString *string = AS_STRING(value);
      return sbAppend(builder, "'", 1) &&
             sbAppend(builder, string->chars, (size_t)string->length) &&
             sbAppend(builder, "'", 1);
    }
  }

  /* Only scalars reach here; containers were handled above. Recursing into
   * csValueToCString for a container would loop forever. */
  size_t length = 0;
  char *text = csValueToCString(value, &length);
  if (text == NULL) return false;
  bool ok = sbAppend(builder, text, length);
  free(text);
  return ok;
}

char *csValueInspect(Value value, size_t *lengthOut) {
  StringBuilder builder = {NULL, 0, 0};
  /* quoteStrings is false at the top level and true inside containers, which
   * sbAppendArray and sbAppendObject arrange for themselves. */
  if (!sbAppendValue(&builder, value, IS_NUMBER(value))) {
    free(builder.data);
    return NULL;
  }
  if (lengthOut != NULL) *lengthOut = builder.length;
  return builder.data;
}

/* An array converted to a *string* is its elements joined with commas, and
 * nested arrays flatten — `String([1, [2, 3]])` is "1,2,3". That is what
 * JavaScript does, and it is what a template literal or a `+` produces.
 *
 * Printing is a different question: console.log shows `[ 1, 2 ]` so a nested
 * structure stays readable. The two paths deliberately disagree. */
static bool sbAppendArrayAsString(StringBuilder *builder, ObjArray *array) {
  for (int i = 0; i < array->elements.count; i++) {
    if (i > 0 && !sbAppend(builder, ",", 1)) return false;

    Value element = array->elements.values[i];
    /* null and undefined join as nothing at all, the same as an empty slot. */
    if (IS_NULL(element) || IS_UNDEFINED(element)) continue;

    if (IS_ARRAY(element)) {
      if (!sbAppendArrayAsString(builder, AS_ARRAY(element))) return false;
      continue;
    }

    size_t length = 0;
    char *text = csValueToCString(element, &length);
    if (text == NULL) return false;
    bool ok = sbAppend(builder, text, length);
    free(text);
    if (!ok) return false;
  }
  return true;
}

char *csValueToCString(Value value, size_t *lengthOut) {
  char buffer[32];
  const char *text = buffer;
  size_t length;

  if (IS_NUMBER(value)) {
    length = (size_t)csValueFormatNumber(buffer, sizeof(buffer), AS_NUMBER(value));
  } else if (IS_BOOL(value)) {
    text = AS_BOOL(value) ? "true" : "false";
    length = AS_BOOL(value) ? 4 : 5;
  } else if (IS_NULL(value)) {
    text = "null";
    length = 4;
  } else if (IS_UNDEFINED(value)) {
    text = "undefined";
    length = 9;
  } else {
      if (IS_STRING(value)) {
        ObjString *string = AS_STRING(value);
        text = string->chars;
        length = (size_t)string->length;
      } else if (IS_CALLABLE(value)) {
        return renderCallable(value, lengthOut);
      } else if (IS_BIGINT(value)) {
        /* Without the trailing `n`: that is how a BigInt is *written*, not
         * what it says when a program asks for its text. `String(1n)` is "1"
         * in JavaScript too, and printing it shows the `n`. */
        char *rendered = csBigToText(&AS_BIGINT(value)->value, 10);
        if (rendered == NULL) return NULL;
        if (lengthOut != NULL) *lengthOut = strlen(rendered);
        return rendered;
      } else if (IS_SYMBOL(value)) {
        /* `String(symbol)` is `Symbol(description)`. JavaScript throws for an
         * implicit conversion and allows the explicit one; here there is one
         * conversion and it always says what the value is. */
        ObjSymbol *symbol = AS_SYMBOL(value);
        const char *described =
            symbol->description != NULL ? symbol->description->chars : "";
        size_t needed = strlen(described) + 10;
        char *rendered = (char *)malloc(needed);
        if (rendered == NULL) return NULL;
        int written = snprintf(rendered, needed, "Symbol(%s)", described);
        if (lengthOut != NULL) *lengthOut = (size_t)written;
        return rendered;
      } else if (IS_DATE(value)) {
        /* The ISO form, which is the one a Date has that does not depend on
         * where the program is running. JavaScript's `String(date)` gives a
         * local, human, locale-shaped string instead; this one is the same
         * everywhere, and is what `toISOString` and JSON already produce. */
        char iso[64];
        if (!csDateToISO(AS_DATE(value)->ms, iso, sizeof iso)) {
          text = "Invalid Date";
          length = 12;
        } else {
          size_t isoLength = strlen(iso);
          char *copy = (char *)malloc(isoLength + 1);
          if (copy == NULL) return NULL;
          memcpy(copy, iso, isoLength + 1);
          if (lengthOut != NULL) *lengthOut = isoLength;
          return copy;
        }
      } else if (!IS_ARRAY(value) && !IS_OBJECT(value) && !IS_PROMISE(value) &&
                 !IS_MAP(value) && !IS_REGEX(value)) {
        /* A heap type with no rendering of its own — a module, a shape, a
         * fiber. Naming it stops the fall-through below from calling back into
         * this function and recursing until the stack runs out, which is what
         * a new object type used to do until someone added a case here. */
        text = "[internal]";
        length = 10;
      } else {
        StringBuilder builder = {NULL, 0, 0};
        bool ok = IS_ARRAY(value) ? sbAppendArrayAsString(&builder, AS_ARRAY(value))
                                  : sbAppendValue(&builder, value, false);
        if (!ok) {
          free(builder.data);
          return NULL;
        }
        /* An empty array joins to an empty string, which writes nothing — so
         * the builder never allocated and there is no buffer to hand back. */
        if (builder.data == NULL) {
          builder.data = (char *)malloc(1);
          if (builder.data == NULL) return NULL;
          builder.data[0] = '\0';
          builder.length = 0;
        }
        if (lengthOut != NULL) *lengthOut = builder.length;
        return builder.data;
      }
  }

  char *result = (char *)malloc(length + 1);
  if (result == NULL) return NULL;
  memcpy(result, text, length);
  result[length] = '\0';
  if (lengthOut != NULL) *lengthOut = length;
  return result;
}
