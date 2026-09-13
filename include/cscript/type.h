/* type.h — the static type lattice.
 *
 * Types are deliberately shallow: a fixed set of kinds, no structural or
 * higher-order types, no generics. That is what keeps checking cheap, and it
 * is also what shapes the two types at the top of the list below.
 *
 * There is no `any`. A type a program can *write* is a type the checker
 * enforces — that is the whole rule. Two kinds exist that are not exact, and
 * they are not the same thing:
 *
 *   `unknown` is written by programs. Everything converts *to* it and nothing
 *             converts *out* of it: an `unknown` must be narrowed with `typeof`
 *             before it can be used. It is what a container holds when it
 *             cannot say what it holds, which is what a language without
 *             generics needs instead of an escape hatch. TypeScript spells the
 *             same idea the same way.
 *
 *   DYNAMIC   cannot be written at all. It is the checker admitting it does
 *             not know — the type of a property read, of an element of an
 *             array, of a call through a variable. It is permissive, because
 *             refusing everything the checker cannot see would refuse most
 *             programs. The boundary is checked at runtime instead.
 *
 * An unannotated declaration takes the type of its initialiser and keeps it:
 * `let n = 0` is a number from then on, and assigning a string to it is a
 * compile error. A parameter has no initialiser to learn from, so it must be
 * annotated.
 */
#ifndef CSCRIPT_TYPE_H
#define CSCRIPT_TYPE_H

#include "cscript/common.h"

typedef enum {
  /* What the checker could not work out. Assignable in both directions, and
   * impossible to write in a program — see the header. */
  TYPE_DYNAMIC,

  /* The top type, and the one a program writes when it means "some value".
   * Everything is assignable to it; nothing is assignable from it without a
   * `typeof` check first. */
  TYPE_UNKNOWN,

  TYPE_NUMBER,
  /* Whole numbers with no upper bound. Deliberately *not* a subtype of number
   * and not assignable to one: mixing the two is the mistake BigInt exists to
   * make impossible, so the checker refuses it rather than widening. */
  TYPE_BIGINT,
  TYPE_STRING,
  TYPE_BOOLEAN,
  TYPE_NULL,
  TYPE_UNDEFINED,
  /* Written `Function`, as TypeScript writes it: `function` is a keyword, and
   * a keyword cannot stand in a type position in TypeScript's grammar — which
   * a CScript file has to stay inside to keep its second reader, Node. */
  TYPE_FUNCTION,
  /* An array is an object in the runtime and a separate type here, because the
   * two answer different questions: `xs.push` is an array method and `o.push`
   * is almost certainly a mistake. An array is assignable to `object`, as it
   * is in JavaScript; an object is not assignable to an array. */
  TYPE_ARRAY,
  TYPE_OBJECT,

  /* An error was already reported for this expression. It absorbs every
   * operation silently, so one bad subexpression does not produce a cascade of
   * complaints about everything built on top of it. */
  TYPE_ERROR,

  /* Every value from here on names a declared interface: the nth one declared
   * in a file is TYPE_INTERFACE_FIRST + n.
   *
   * Encoded inside the enum rather than carried in a struct beside it because
   * a TypeKind is passed by value through every function in the checker and
   * stored in a byte on a compiled function. A type *tree* is what this
   * becomes when generics or unions need one, and deliberately not before. */
  TYPE_INTERFACE_FIRST = 32,
} TypeKind;

/* Per file, because types do not cross a module boundary. */
#define CS_MAX_INTERFACES 48
#define CS_MAX_INTERFACE_MEMBERS 24
#define CS_MAX_TYPE_ALIASES 48

/* One property of an interface. A method is a member like any other — its type
 * is Function — with the return type recorded so that `shape.area()` answers
 * something better than "a call, contents unknown". */
typedef struct {
  const char *name; /* NUL-terminated, owned by whoever registered it */
  int length;
  TypeKind type;
  TypeKind returns;
  bool isMethod;
  /* `x?: number`. A missing key satisfies the interface; a present one still
   * has to have the right type. */
  bool optional;
} InterfaceMember;

typedef struct {
  const char *name;
  int length;
  InterfaceMember members[CS_MAX_INTERFACE_MEMBERS];
  int memberCount;
} InterfaceType;

/* The interfaces and aliases one file declared. Built by the parser, read by
 * the checker, and hung off the program node so the two need not share
 * anything else — which is what keeps a module compiled inside another
 * module's compile from overwriting it. */
typedef struct {
  InterfaceType interfaces[CS_MAX_INTERFACES];
  int interfaceCount;

  struct {
    const char *name;
    int length;
    TypeKind type;
  } aliases[CS_MAX_TYPE_ALIASES];
  int aliasCount;
} TypeRegistry;

static inline bool csTypeIsInterface(TypeKind type) {
  return type >= TYPE_INTERFACE_FIRST;
}

static inline TypeKind csTypeInterfaceAt(int index) {
  return (TypeKind)(TYPE_INTERFACE_FIRST + index);
}

static inline int csTypeInterfaceIndex(TypeKind type) {
  return (int)type - (int)TYPE_INTERFACE_FIRST;
}

/* The name used in messages and accepted in annotations. */
const char *csTypeName(TypeKind type);

/* Parses an annotation like `number`. Returns false when the name is not one.
 * `any` is deliberately not a name: see csTypeRejectedName. */
bool csTypeFromName(const char *name, int length, TypeKind *out);

/* A name that is refused on purpose, and why — or NULL when the name is simply
 * unknown. Keeps the message for `any` in one place. */
const char *csTypeRejectedName(const char *name, int length);

/* Can a value of `from` be stored where `to` is expected? */
bool csTypeAssignable(TypeKind from, TypeKind to);

/* True when the type is known well enough to specialise code for it. */
static inline bool csTypeIsKnown(TypeKind type) {
  return type != TYPE_DYNAMIC && type != TYPE_ERROR;
}

/* True when using the value at all — arithmetic, a property, a call — needs a
 * `typeof` check first. */
static inline bool csTypeNeedsNarrowing(TypeKind type) {
  return type == TYPE_UNKNOWN;
}

/* The type a `typeof` result names, for narrowing: `"number"` gives
 * TYPE_NUMBER. False when the text names nothing. */
bool csTypeFromTypeofName(const char *name, int length, TypeKind *out);

/* --- declared types ------------------------------------------------------ */

void csTypeRegistryInit(TypeRegistry *types);

/* Declares an interface and answers its type, or TYPE_ERROR when the file has
 * more than the ceiling above. The name must outlive the registry. */
TypeKind csTypeDeclareInterface(TypeRegistry *types, const char *name, int length);

/* False when the interface is full or already has a member of that name. */
bool csTypeInterfaceAddMember(TypeRegistry *types, TypeKind type, const InterfaceMember *member);

bool csTypeDeclareAlias(TypeRegistry *types, const char *name, int length, TypeKind type);

/* A name written in an annotation: a built-in type, then an alias, then an
 * interface. False when nothing of that name has been declared. */
bool csTypeLookupName(const TypeRegistry *types, const char *name, int length, TypeKind *out);

/* The interface a type names, or NULL when it names something else. */
const InterfaceType *csTypeInterface(const TypeRegistry *types, TypeKind type);

const InterfaceMember *csTypeFindInterfaceMember(const TypeRegistry *types, TypeKind type, const char *name, int length);

/* Like csTypeName, but able to name a declared interface. Every message the
 * checker prints goes through this one. */
const char *csTypeNameIn(const TypeRegistry *types, TypeKind type);

/* Like csTypeAssignable, and additionally structural where an interface is
 * involved: one interface satisfies another when it has every member the
 * other requires, with an assignable type. */
bool csTypeAssignableIn(const TypeRegistry *types, TypeKind from, TypeKind to);

#endif /* CSCRIPT_TYPE_H */
