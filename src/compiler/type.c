/* type.c — the type names, and what may be assigned to what.
 *
 * TypeKind is a flat enum, and this is the whole of the relation over it. Two
 * rules do the work, and they are deliberately not symmetrical:
 *
 *   Everything is assignable **to** `value`, and nothing is assignable **from**
 *   it. That is what makes `value` a type a program can trust: holding one
 *   proves nothing about what is inside, so it has to be narrowed before use.
 *
 *   DYNAMIC flows both ways. That is not an escape hatch a program can reach
 *   for — it cannot be written — but the place where the checker admits it
 *   cannot see, and the runtime checks instead.
 */
#include <string.h>

#include "cscript/type.h"

const char *csTypeName(TypeKind type) {
  switch (type) {
    case TYPE_DYNAMIC: return "unknown";
    case TYPE_VALUE: return "value";
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
  }
  return "<unknown>";
}

static bool named(const char *name, int length, const char *candidate) {
  return (int)strlen(candidate) == length && memcmp(candidate, name, (size_t)length) == 0;
}

bool csTypeFromName(const char *name, int length, TypeKind *out) {
  static const struct {
    const char *name;
    TypeKind type;
  } table[] = {
      {"value", TYPE_VALUE}, {"number", TYPE_NUMBER},       {"bigint", TYPE_BIGINT},     {"string", TYPE_STRING}, {"boolean", TYPE_BOOLEAN},
      {"null", TYPE_NULL},   {"undefined", TYPE_UNDEFINED}, {"Function", TYPE_FUNCTION}, {"array", TYPE_ARRAY},   {"object", TYPE_OBJECT},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!named(name, length, table[i].name)) continue;
    *out = table[i].type;
    return true;
  }
  return false;
}

const char *csTypeRejectedName(const char *name, int length) {
  /* `any` is the one name a reader will reach for first, and the one the
   * language is built on not having. Saying what to write instead is worth
   * more than saying the name is unknown. */
  if (named(name, length, "any")) {
    return "there is no 'any' in CScript: write 'value' and narrow it with "
           "typeof, or name the type";
  }
  if (named(name, length, "unknown")) {
    return "'unknown' is the checker's own name for what it could not work "
           "out, and cannot be written: use 'value'";
  }
  if (named(name, length, "void")) {
    return "a function that answers nothing needs no return type at all";
  }
  if (named(name, length, "int") || named(name, length, "float") || named(name, length, "double")) {
    return "there is one numeric type, and it is called 'number'";
  }
  if (named(name, length, "bool")) return "the type is called 'boolean'";
  /* `function` is a keyword, and TypeScript's grammar has no place for one in
   * a type. Its own name for a callable is `Function`, and taking that name
   * keeps a CScript program readable by `node --experimental-strip-types`. */
  if (named(name, length, "function")) {
    return "the callable type is spelled 'Function', as in TypeScript: "
           "'function' is a keyword and cannot be a type name";
  }
  if (named(name, length, "str")) return "the type is called 'string'";
  return NULL;
}

bool csTypeAssignable(TypeKind from, TypeKind to) {
  /* Anything already in error absorbs further complaints. */
  if (from == TYPE_ERROR || to == TYPE_ERROR) return true;

  /* What the checker could not see. The runtime checks this boundary, which is
   * the one place the system is not static. */
  if (from == TYPE_DYNAMIC || to == TYPE_DYNAMIC) return true;

  /* Widening into the top type, which is always safe: the receiver has learned
   * nothing and the checker will make it prove what it has before it is used. */
  if (to == TYPE_VALUE) return true;

  /* And the other direction is exactly what is refused. `value` means "some
   * type", and storing it where a number is expected would be a claim nothing
   * has checked. */
  if (from == TYPE_VALUE) return false;

  /* An array is an object, as in JavaScript. Not the other way round: an
   * object has no length and no push, and calling one an array is how a
   * program comes to read `undefined` off the end of something. */
  if (from == TYPE_ARRAY && to == TYPE_OBJECT) return true;

  return from == to;
}

bool csTypeFromTypeofName(const char *name, int length, TypeKind *out) {
  /* The strings `typeof` answers with, which are not quite the type names: it
   * says "object" for an array and has no word for `value`. Narrowing on
   * "object" therefore proves only that much — an array reaches the same
   * branch, and asking Object.kindOf is how the two are told apart. */
  static const struct {
    const char *name;
    TypeKind type;
  } table[] = {
      {"number", TYPE_NUMBER},     {"string", TYPE_STRING},       {"boolean", TYPE_BOOLEAN}, {"bigint", TYPE_BIGINT},
      {"function", TYPE_FUNCTION}, {"undefined", TYPE_UNDEFINED}, {"object", TYPE_OBJECT},   {"null", TYPE_NULL},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!named(name, length, table[i].name)) continue;
    *out = table[i].type;
    return true;
  }
  return false;
}
