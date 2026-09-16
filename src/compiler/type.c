/* type.c — the type names, the table behind the composite ones, and what may
 * be assigned to what.
 *
 * Two rules do most of the work, and they are deliberately not symmetrical:
 *
 *   Everything is assignable **to** `unknown`, and nothing is assignable
 *   **from** it. That is what makes `unknown` a type a program can trust:
 *   holding one proves nothing about what is inside, so it has to be narrowed
 *   before use.
 *
 *   DYNAMIC flows both ways. That is not an escape hatch a program can reach
 *   for — it cannot be written — but the place where the checker admits it
 *   cannot see, and the runtime checks instead.
 *
 * Everything below those is structural: an interface is satisfied by what a
 * value has, an array by what it holds, a function by what it takes and
 * answers. Nominal typing would need a notion of identity that survives a
 * module boundary; structural typing needs nothing at all.
 */
#include <stdio.h>
#include <string.h>

#include "compiler/type_internal.h"

const char *csTypeName(TypeId type) {
  switch (type) {
    case TYPE_DYNAMIC: return "untyped";
    case TYPE_UNKNOWN: return "unknown";
    case TYPE_NUMBER: return "number";
    case TYPE_BIGINT: return "bigint";
    case TYPE_STRING: return "string";
    case TYPE_BOOLEAN: return "boolean";
    case TYPE_NULL: return "null";
    case TYPE_UNDEFINED: return "undefined";
    case TYPE_FUNCTION: return "Function";
    case TYPE_ARRAY: return "array";
    case TYPE_OBJECT: return "object";
    case TYPE_ERROR: return "<error>";
    default: break;
  }
  /* A composite has a name only its own table knows; csTypeNameIn is the one
   * that can say it, and every message the checker prints goes through it. */
  return csTypeIsComposite(type) ? "a declared type" : "<untyped>";
}

bool csTypeNameMatches(const char *name, int length, const char *candidate) {
  return (int)strlen(candidate) == length && memcmp(candidate, name, (size_t)length) == 0;
}

bool csTypeFromName(const char *name, int length, TypeId *out) {
  static const struct {
    const char *name;
    TypeId type;
  } table[] = {
      {"unknown", TYPE_UNKNOWN},
      {"number", TYPE_NUMBER},
      {"bigint", TYPE_BIGINT},
      {"string", TYPE_STRING},
      {"boolean", TYPE_BOOLEAN},
      {"null", TYPE_NULL},
      {"undefined", TYPE_UNDEFINED},
      {"Function", TYPE_FUNCTION},
      {"array", TYPE_ARRAY},
      /* TypeScript's other spelling of the same thing. `Array<T>` becomes a
       * `T[]` where it is parsed, and a bare `Array` is a bare `array`. */
      {"Array", TYPE_ARRAY},
      {"object", TYPE_OBJECT},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!csTypeNameMatches(name, length, table[i].name)) continue;
    *out = table[i].type;
    return true;
  }
  return false;
}

const char *csTypeRejectedName(const char *name, int length) {
  /* `any` is the one name a reader will reach for first, and the one the
   * language is built on not having. Saying what to write instead is worth
   * more than saying the name is unknown. */
  if (csTypeNameMatches(name, length, "any")) {
    return "there is no 'any' in CScript: write 'unknown' and narrow it with "
           "typeof, or name the type";
  }
  /* The name this language used for its top type before it took TypeScript's. */
  if (csTypeNameMatches(name, length, "value")) {
    return "the type that holds anything is called 'unknown', as in TypeScript";
  }
  if (csTypeNameMatches(name, length, "void")) {
    return "a function that answers nothing needs no return type at all";
  }
  if (csTypeNameMatches(name, length, "int") || csTypeNameMatches(name, length, "float") || csTypeNameMatches(name, length, "double")) {
    return "there is one numeric type, and it is called 'number'";
  }
  if (csTypeNameMatches(name, length, "bool")) return "the type is called 'boolean'";
  /* `function` is a keyword, and TypeScript's grammar has no place for one in
   * a type. Its own name for a callable is `Function`, and taking that name
   * keeps a CScript program readable by `node --experimental-strip-types`. */
  if (csTypeNameMatches(name, length, "function")) {
    return "the callable type is spelled 'Function', as in TypeScript: "
           "'function' is a keyword and cannot be a type name";
  }
  if (csTypeNameMatches(name, length, "str")) return "the type is called 'string'";
  return NULL;
}

bool csTypeAssignable(TypeId from, TypeId to) {
  /* Anything already in error absorbs further complaints. */
  if (from == TYPE_ERROR || to == TYPE_ERROR) return true;

  /* What the checker could not see. The runtime checks this boundary, which is
   * the one place the system is not static. */
  if (from == TYPE_DYNAMIC || to == TYPE_DYNAMIC) return true;

  /* Widening into the top type, which is always safe: the receiver has learned
   * nothing and the checker will make it prove what it has before it is used. */
  if (to == TYPE_UNKNOWN) return true;

  /* And the other direction is exactly what is refused. `unknown` means "some
   * type", and storing it where a number is expected would be a claim nothing
   * has checked. */
  if (from == TYPE_UNKNOWN) return false;

  /* An array is an object, as in JavaScript. Not the other way round: an
   * object has no length and no push, and calling one an array is how a
   * program comes to read `undefined` off the end of something. */
  if (from == TYPE_ARRAY && to == TYPE_OBJECT) return true;

  return from == to;
}

bool csTypeFromTypeofName(const char *name, int length, TypeId *out) {
  /* The strings `typeof` answers with, which are not quite the type names: it
   * says "object" for an array and has no word for `unknown`. Narrowing on
   * "object" therefore proves only that much — an array reaches the same
   * branch, and Array.isArray is how the two are told apart. */
  static const struct {
    const char *name;
    TypeId type;
  } table[] = {
      {"number", TYPE_NUMBER},     {"string", TYPE_STRING},       {"boolean", TYPE_BOOLEAN}, {"bigint", TYPE_BIGINT},
      {"function", TYPE_FUNCTION}, {"undefined", TYPE_UNDEFINED}, {"object", TYPE_OBJECT},   {"null", TYPE_NULL},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!csTypeNameMatches(name, length, table[i].name)) continue;
    *out = table[i].type;
    return true;
  }
  return false;
}

/* --- assignability ------------------------------------------------------- */

static bool assignable(const TypeTable *table, TypeId from, TypeId to, int depth);

/* Structural, as in TypeScript: what matters is what a value has, not what it
 * was declared as. An interface is satisfied by anything carrying every member
 * it requires — which is the only rule that lets two files describe the same
 * shape without one of them importing the other. */
static bool satisfies(const TypeTable *table, TypeId from, TypeId to, int depth) {
  const CompositeType *required = csTypeComposite(table, to);
  if (required == NULL) return false;

  for (int i = 0; i < required->memberCount; i++) {
    const TypeMember *want = &table->members[required->memberStart + i];
    const TypeMember *have = csTypeFindMember(table, from, want->name, want->length);
    if (have == NULL) {
      if (want->optional) continue;
      return false;
    }
    if (depth > 6) continue; /* a shape that refers to itself is taken as given */
    if (!assignable(table, have->type, want->type, depth + 1)) return false;
  }
  return true;
}

static bool assignable(const TypeTable *table, TypeId from, TypeId to, int depth) {
  if (from == to) return true;

  bool fromComposite = csTypeIsComposite(from);
  bool toComposite = csTypeIsComposite(to);
  if (!fromComposite && !toComposite) return csTypeAssignable(from, to);

  if (from == TYPE_ERROR || to == TYPE_ERROR) return true;
  if (from == TYPE_DYNAMIC || to == TYPE_DYNAMIC) return true;
  if (to == TYPE_UNKNOWN) return true;
  if (from == TYPE_UNKNOWN) return false;
  if (depth > 8) return true;

  const CompositeType *source = csTypeComposite(table, from);
  const CompositeType *target = csTypeComposite(table, to);

  /* A union on the left has to fit whole: every member of it must be
   * acceptable, because the value is any one of them. */
  if (source != NULL && source->kind == COMPOSITE_UNION) {
    for (int i = 0; i < source->slotCount; i++) {
      if (!assignable(table, table->slots[source->slotStart + i], to, depth + 1)) return false;
    }
    return true;
  }

  /* A union on the right needs one member to fit, which is what makes
   * `T | null` the type of something that may be absent. */
  if (target != NULL && target->kind == COMPOSITE_UNION) {
    for (int i = 0; i < target->slotCount; i++) {
      if (assignable(table, from, table->slots[target->slotStart + i], depth + 1)) return true;
    }
    return false;
  }

  if (source != NULL && source->kind == COMPOSITE_ARRAY) {
    /* `number[]` is an array and an object, as a bare `array` is. */
    if (to == TYPE_ARRAY || to == TYPE_OBJECT) return true;
    if (target == NULL || target->kind != COMPOSITE_ARRAY) return false;
    /* Covariant in the element, as TypeScript is: unsound for a write through
     * an aliased array, and the alternative is refusing `Shape[]` where
     * `Point[]` is wanted, which is the case that actually comes up. */
    return assignable(table, source->inner, target->inner, depth + 1);
  }
  if (target != NULL && target->kind == COMPOSITE_ARRAY) {
    /* A bare `array` says nothing about its elements, so it is accepted the
     * way DYNAMIC is: a read off it is checked where it lands. */
    return from == TYPE_ARRAY;
  }

  if (source != NULL && source->kind == COMPOSITE_FUNCTION) {
    if (to == TYPE_FUNCTION || to == TYPE_OBJECT) return true;
    if (target == NULL || target->kind != COMPOSITE_FUNCTION) return false;

    /* A function may be used where one taking *more* arguments is wanted: the
     * extra ones are simply not read, which is why `xs.map(x => x)` is legal
     * against a callback of three parameters. The reverse is not — a callee
     * that needs an argument the caller never passes is a mistake. */
    if (source->requiredCount > target->slotCount) return false;

    /* Contravariant in the parameters and covariant in the result, which is
     * the sound direction: whatever the caller passes must be acceptable to
     * this function, and whatever this function answers must be acceptable to
     * the caller. */
    int shared = source->slotCount < target->slotCount ? source->slotCount : target->slotCount;
    for (int i = 0; i < shared; i++) {
      TypeId mine = table->slots[source->slotStart + i];
      TypeId theirs = table->slots[target->slotStart + i];
      if (!assignable(table, theirs, mine, depth + 1)) return false;
    }
    return assignable(table, source->inner, target->inner, depth + 1);
  }
  if (target != NULL && target->kind == COMPOSITE_FUNCTION) {
    /* A bare `Function` is callable with a signature nobody wrote down. */
    return from == TYPE_FUNCTION;
  }

  /* A type variable is only itself. Inside the declaration that introduced it
   * nothing else is known about it, and that is the point: a generic body is
   * checked once, for every instantiation at once. */
  if ((source != NULL && source->kind == COMPOSITE_TYPEVAR) || (target != NULL && target->kind == COMPOSITE_TYPEVAR)) return false;

  if (source != NULL && source->kind == COMPOSITE_INTERFACE) {
    if (to == TYPE_OBJECT) return true;
    if (target == NULL || target->kind != COMPOSITE_INTERFACE) return false;
    return satisfies(table, from, to, depth);
  }

  /* And nothing shapeless satisfies a shape. An `object` is an object whose
   * members are not modelled, so calling it a Point would be a claim nothing
   * checked — the same claim `any` used to let a program make. */
  return false;
}

bool csTypeAssignableIn(const TypeTable *table, TypeId from, TypeId to) {
  return assignable(table, from, to, 0);
}
