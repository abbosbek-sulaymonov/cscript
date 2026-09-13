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
    /* An interface has a name only its own file knows; csTypeNameIn is the one
     * that can say it, and every message the checker prints goes through
     * there. */
    case TYPE_INTERFACE_FIRST: break;
  }
  return csTypeIsInterface(type) ? "an interface" : "<untyped>";
}

static bool named(const char *name, int length, const char *candidate) {
  return (int)strlen(candidate) == length && memcmp(candidate, name, (size_t)length) == 0;
}

bool csTypeFromName(const char *name, int length, TypeKind *out) {
  static const struct {
    const char *name;
    TypeKind type;
  } table[] = {
      {"unknown", TYPE_UNKNOWN}, {"number", TYPE_NUMBER},       {"bigint", TYPE_BIGINT},     {"string", TYPE_STRING}, {"boolean", TYPE_BOOLEAN},
      {"null", TYPE_NULL},       {"undefined", TYPE_UNDEFINED}, {"Function", TYPE_FUNCTION}, {"array", TYPE_ARRAY},   {"object", TYPE_OBJECT},
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
    return "there is no 'any' in CScript: write 'unknown' and narrow it with "
           "typeof, or name the type";
  }
  /* The name this language used for its top type before it took TypeScript's.
   * Worth answering rather than merely rejecting, because it is the one a
   * reader of older CScript will write. */
  if (named(name, length, "value")) {
    return "the type that holds anything is called 'unknown', as in TypeScript";
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
  if (to == TYPE_UNKNOWN) return true;

  /* And the other direction is exactly what is refused. `value` means "some
   * type", and storing it where a number is expected would be a claim nothing
   * has checked. */
  if (from == TYPE_UNKNOWN) return false;

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

/* --- declared types ------------------------------------------------------ */

void csTypeRegistryInit(TypeRegistry *types) {
  types->interfaceCount = 0;
  types->aliasCount = 0;
}

TypeKind csTypeDeclareInterface(TypeRegistry *types, const char *name, int length) {
  if (types->interfaceCount >= CS_MAX_INTERFACES) return TYPE_ERROR;

  InterfaceType *declared = &types->interfaces[types->interfaceCount];
  declared->name = name;
  declared->length = length;
  declared->memberCount = 0;
  return csTypeInterfaceAt(types->interfaceCount++);
}

const InterfaceType *csTypeInterface(const TypeRegistry *types, TypeKind type) {
  if (types == NULL || !csTypeIsInterface(type)) return NULL;
  int index = csTypeInterfaceIndex(type);
  if (index >= types->interfaceCount) return NULL;
  return &types->interfaces[index];
}

bool csTypeInterfaceAddMember(TypeRegistry *types, TypeKind type, const InterfaceMember *member) {
  const InterfaceType *found = csTypeInterface(types, type);
  if (found == NULL) return false;

  InterfaceType *declared = &types->interfaces[csTypeInterfaceIndex(type)];
  if (declared->memberCount >= CS_MAX_INTERFACE_MEMBERS) return false;
  for (int i = 0; i < declared->memberCount; i++) {
    if (named(member->name, member->length, declared->members[i].name)) return false;
  }
  declared->members[declared->memberCount++] = *member;
  return true;
}

bool csTypeDeclareAlias(TypeRegistry *types, const char *name, int length, TypeKind type) {
  if (types->aliasCount >= CS_MAX_TYPE_ALIASES) return false;
  types->aliases[types->aliasCount].name = name;
  types->aliases[types->aliasCount].length = length;
  types->aliases[types->aliasCount].type = type;
  types->aliasCount++;
  return true;
}

bool csTypeLookupName(const TypeRegistry *types, const char *name, int length, TypeKind *out) {
  if (csTypeFromName(name, length, out)) return true;
  if (types == NULL) return false;

  /* An alias is a second name for a type that already exists, so it is looked
   * up before the interfaces: `type Point2 = Point` records Point's own type
   * rather than a copy of it. */
  for (int i = 0; i < types->aliasCount; i++) {
    if (!named(name, length, types->aliases[i].name)) continue;
    *out = types->aliases[i].type;
    return true;
  }
  for (int i = 0; i < types->interfaceCount; i++) {
    if (!named(name, length, types->interfaces[i].name)) continue;
    *out = csTypeInterfaceAt(i);
    return true;
  }
  return false;
}

const InterfaceMember *csTypeFindInterfaceMember(const TypeRegistry *types, TypeKind type, const char *name, int length) {
  const InterfaceType *declared = csTypeInterface(types, type);
  if (declared == NULL) return NULL;
  for (int i = 0; i < declared->memberCount; i++) {
    if (named(name, length, declared->members[i].name)) return &declared->members[i];
  }
  return NULL;
}

const char *csTypeNameIn(const TypeRegistry *types, TypeKind type) {
  const InterfaceType *declared = csTypeInterface(types, type);
  return declared != NULL ? declared->name : csTypeName(type);
}

/* Structural, as in TypeScript: what matters is what a value has, not what it
 * was declared as. An interface is satisfied by anything carrying every member
 * it requires — which is the only rule that lets two files describe the same
 * shape without one of them importing the other. */
static bool satisfies(const TypeRegistry *types, TypeKind from, TypeKind to) {
  const InterfaceType *required = csTypeInterface(types, to);
  const InterfaceType *given = csTypeInterface(types, from);
  if (required == NULL || given == NULL) return false;

  for (int i = 0; i < required->memberCount; i++) {
    const InterfaceMember *want = &required->members[i];
    const InterfaceMember *have = csTypeFindInterfaceMember(types, from, want->name, want->length);
    if (have == NULL) {
      if (want->optional) continue;
      return false;
    }
    if (!csTypeAssignableIn(types, have->type, want->type)) return false;
  }
  return true;
}

bool csTypeAssignableIn(const TypeRegistry *types, TypeKind from, TypeKind to) {
  /* The same interface, and the self-referential case `interface Node { next:
   * Node }` that would otherwise walk for ever. */
  if (from == to) return true;

  bool fromInterface = csTypeIsInterface(from);
  bool toInterface = csTypeIsInterface(to);
  if (!fromInterface && !toInterface) return csTypeAssignable(from, to);

  /* What the checker could not see flows either way, as it does everywhere. */
  if (from == TYPE_ERROR || to == TYPE_ERROR) return true;
  if (from == TYPE_DYNAMIC || to == TYPE_DYNAMIC) return true;

  /* An interface is a kind of object, and widening to `unknown` is always
   * allowed — it is the narrowing back out that is not. */
  if (to == TYPE_UNKNOWN) return true;
  if (from == TYPE_UNKNOWN) return false;

  if (fromInterface && toInterface) return satisfies(types, from, to);
  if (fromInterface) return to == TYPE_OBJECT;

  /* And nothing shapeless satisfies a shape. An `object` is an object whose
   * members are not modelled, so calling it a Point would be a claim nothing
   * checked — the same claim `any` used to let a program make. */
  return false;
}
