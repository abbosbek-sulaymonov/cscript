/* type.h — what a type is.
 *
 * A type is an **int**. Small values are the primitive kinds in TypeKind
 * below; everything from TYPE_COMPOSITE_FIRST on is an index into the file's
 * TypeTable, where the shapes live — an interface, an array of something, a
 * function from something to something, a union of several.
 *
 * That encoding is the whole design. A type stays one scalar passed by value
 * through every function in the checker and stored in a byte on a compiled
 * function, and the tree hangs off to one side, reached only by the code that
 * needs to look inside. Composites are **interned**: `number[]` written twice
 * is one entry and one id, so comparing two types is usually an integer
 * compare, and structural equality falls out of it.
 *
 * There is no `any`. A type a program can *write* is a type the checker
 * enforces — that is the whole rule. Two kinds are not exact, and they are not
 * the same thing:
 *
 *   `unknown` is written by programs. Everything converts *to* it and nothing
 *             converts *out* of it: an `unknown` must be narrowed with `typeof`
 *             before it can be used. TypeScript spells the same idea the same
 *             way.
 *
 *   DYNAMIC   cannot be written at all. It is the checker admitting it does
 *             not know — what an import holds, what a plain object's property
 *             is. It is permissive, because refusing everything the checker
 *             cannot see would refuse most programs. The boundary is checked
 *             at run time instead.
 *
 * An unannotated declaration takes the type of its initialiser and keeps it:
 * `let n = 0` is a number from then on, and assigning a string to it is a
 * compile error. A parameter has no initialiser to learn from, so it must be
 * annotated.
 */
#ifndef CSCRIPT_TYPE_H
#define CSCRIPT_TYPE_H

#include "cscript/common.h"

/* The primitive kinds, and the low ids. A TypeId less than TYPE_COMPOSITE_FIRST
 * is one of these and nothing else, which is what lets the compiler keep asking
 * `type == TYPE_NUMBER` without knowing a table exists. */
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
   * a CScript file has to stay inside to keep its second reader, Node. A
   * *typed* function is a composite; this is the one that says only "callable".
   */
  TYPE_FUNCTION,
  /* An array whose element type is not known — what `array` means when it is
   * written bare. `T[]` is a composite, and knows. */
  TYPE_ARRAY,
  TYPE_OBJECT,

  /* An error was already reported for this expression. It absorbs every
   * operation silently, so one bad subexpression does not produce a cascade of
   * complaints about everything built on top of it. */
  TYPE_ERROR,

  /* Every id from here on indexes the file's TypeTable. */
  TYPE_COMPOSITE_FIRST = 32,
} TypeKind;

/* A type: a primitive kind, or an index into the table. */
typedef int TypeId;

/* Per file, because a type table belongs to the file that built it. */
#define CS_MAX_COMPOSITE_TYPES 256
#define CS_MAX_TYPE_MEMBERS 768
#define CS_MAX_TYPE_SLOTS 768
#define CS_MAX_TYPE_ALIASES 64
#define CS_MAX_TYPE_NAME_CHARS 8192
#define CS_MAX_TYPE_PARAMS 4

typedef enum {
  /* A declared shape: `interface Point { x: number }`, or the same written
   * inline as `type Point = { x: number }`. Compared structurally. */
  COMPOSITE_INTERFACE,
  /* `T[]` — an array that knows what it holds. */
  COMPOSITE_ARRAY,
  /* `(a: number) => string`. */
  COMPOSITE_FUNCTION,
  /* `A | B` — including the two that make the rest of the system honest,
   * `T | null` and `T | undefined`. */
  COMPOSITE_UNION,
  /* `T` inside the declaration that introduced it. Erased at the call site by
   * instantiation; a leftover one is the checker admitting it could not infer
   * what the caller meant. */
  COMPOSITE_TYPEVAR,
} CompositeKind;

/* One member of an interface. A method is a member like any other — its type
 * is a function type — so a call through it is checked the same way a call
 * through a variable is. */
typedef struct {
  const char *name; /* NUL-terminated, owned by whoever registered it */
  int length;
  TypeId type;
  /* `x?: number`. A missing key satisfies the interface; a present one still
   * has to have the right type. */
  bool optional;
} TypeMember;

typedef struct {
  CompositeKind kind;

  /* An interface's name, a type variable's name, or NULL. */
  const char *name;
  int nameLength;

  /* COMPOSITE_INTERFACE: a run of the member pool. */
  int memberStart;
  int memberCount;

  /* COMPOSITE_ARRAY: what it holds.
   * COMPOSITE_FUNCTION: what it answers. */
  TypeId inner;

  /* COMPOSITE_FUNCTION: a run of the slot pool holding parameter types.
   * COMPOSITE_UNION: a run of the slot pool holding the members.
   * An instantiated generic: a run holding the type arguments. */
  int slotStart;
  int slotCount;

  /* COMPOSITE_FUNCTION: how many parameters a call must supply, and whether
   * the last one collects the rest. */
  int requiredCount;
  bool hasRest;

  /* A declaration's own type parameters — `interface Box<T>`, `function
   * first<T>(...)`. They are TYPE_COMPOSITE typevars, in declaration order. */
  TypeId typeParams[CS_MAX_TYPE_PARAMS];
  int typeParamCount;

  /* For an instantiation — `Box<number>` — the generic it came from, so a
   * message can say which. TYPE_DYNAMIC when this is not one. */
  TypeId genericOf;

  /* A type variable is only in scope inside the declaration that introduced
   * it. Ids are never reused, so the parser closes the scope by clearing this
   * rather than by removing the entry. */
  bool active;

  /* A class's shape, which is **open**: a constructor may add a field that
   * was never declared — `this.name = name` is the commonest line in one — so
   * reading a member the class did not declare answers what the checker could
   * not work out rather than an error. What it declares is still checked, and
   * still has to be there for the instance to satisfy an interface. */
  bool open;
} CompositeType;

/* Everything one file's types are made of. Built by the parser, read by the
 * checker, and hung off the program node so that a module parsed in the middle
 * of another module's compile has its own. */
typedef struct {
  CompositeType composites[CS_MAX_COMPOSITE_TYPES];
  int compositeCount;

  TypeMember members[CS_MAX_TYPE_MEMBERS];
  int memberCount;

  TypeId slots[CS_MAX_TYPE_SLOTS];
  int slotCount;

  struct {
    const char *name;
    int length;
    TypeId type;
  } aliases[CS_MAX_TYPE_ALIASES];
  int aliasCount;

  /* Every name this table uses, copied in. The table outlives the arena that
   * parsed the file — a module's exports are read long afterwards — so a name
   * pointing at the source text would dangle exactly when it is needed. */
  char names[CS_MAX_TYPE_NAME_CHARS];
  int nameCount;

  /* Set once the table is full, so the overflow is reported once rather than
   * at every type that follows it. */
  bool full;
} TypeTable;

static inline bool csTypeIsComposite(TypeId type) {
  return type >= TYPE_COMPOSITE_FIRST;
}

static inline TypeId csTypeCompositeAt(int index) {
  return (TypeId)(TYPE_COMPOSITE_FIRST + index);
}

static inline int csTypeCompositeIndex(TypeId type) {
  return (int)type - TYPE_COMPOSITE_FIRST;
}

/* The name used in messages for a primitive. csTypeNameIn says the rest. */
const char *csTypeName(TypeId type);

/* Parses a built-in annotation name like `number`. Returns false when the name
 * is not one. `any` is deliberately not a name: see csTypeRejectedName. */
bool csTypeFromName(const char *name, int length, TypeId *out);

/* A name that is refused on purpose, and why — or NULL when the name is simply
 * not known. Keeps the message for `any` in one place. */
const char *csTypeRejectedName(const char *name, int length);

/* Can a value of `from` be stored where `to` is expected? The primitive half
 * of the relation; csTypeAssignableIn is the whole of it. */
bool csTypeAssignable(TypeId from, TypeId to);

/* True when the type is known well enough to specialise code for it. */
static inline bool csTypeIsKnown(TypeId type) {
  return type != TYPE_DYNAMIC && type != TYPE_ERROR;
}

/* True when using the value at all — arithmetic, a property, a call — needs a
 * `typeof` check first. */
static inline bool csTypeNeedsNarrowing(TypeId type) {
  return type == TYPE_UNKNOWN;
}

/* The type a `typeof` result names, for narrowing: `"number"` gives
 * TYPE_NUMBER. False when the text names nothing. */
bool csTypeFromTypeofName(const char *name, int length, TypeId *out);

/* --- the table ----------------------------------------------------------- */

void csTypeTableInit(TypeTable *table);

const CompositeType *csTypeComposite(const TypeTable *table, TypeId type);

/* True when the type is a composite of that kind. */
bool csTypeIs(const TypeTable *table, TypeId type, CompositeKind kind);

/* Interning constructors. Each answers an existing id when one already
 * describes the same type, so `number[]` is one type however often it is
 * written. TYPE_DYNAMIC when the table is full — which degrades to "the
 * checker cannot see this" rather than to a wrong answer. */
TypeId csTypeArrayOf(TypeTable *table, TypeId element);
TypeId csTypeFunctionOf(TypeTable *table, const TypeId *params, int paramCount, int requiredCount, bool hasRest, TypeId result);
TypeId csTypeUnionOf(TypeTable *table, const TypeId *members, int count);

/* `T | null` and friends, without the caller building an array. */
TypeId csTypeUnionWith(TypeTable *table, TypeId left, TypeId right);

/* What an array holds, or TYPE_DYNAMIC for a bare `array`. */
TypeId csTypeElementOf(const TypeTable *table, TypeId array);

/* True when `type` is an array of any kind, bare or knowing. */
bool csTypeIsArrayLike(const TypeTable *table, TypeId type);

/* True when `type` is callable: `Function`, a function type, or a union of
 * things that are. */
bool csTypeIsCallable(const TypeTable *table, TypeId type);

/* What calling one answers — the union of the results when it is a union.
 * TYPE_DYNAMIC for a bare `Function`, which says nothing. */
TypeId csTypeResultOf(TypeTable *table, TypeId callable);

/* --- declared types ------------------------------------------------------ */

/* Declares an interface and answers its type, or TYPE_ERROR when the file has
 * more than the ceiling above. The name is copied into the table. */
TypeId csTypeDeclareInterface(TypeTable *table, const char *name, int length);

/* The same, for a class: what it declares is checked, and what it does not is
 * not refused. See CompositeType.open. */
TypeId csTypeDeclareClass(TypeTable *table, const char *name, int length);
bool csTypeIsOpen(const TypeTable *table, TypeId type);

/* False when the interface is full or already has a member of that name. */
bool csTypeAddMember(TypeTable *table, TypeId type, const TypeMember *member);

bool csTypeDeclareAlias(TypeTable *table, const char *name, int length, TypeId type);

/* A name written in an annotation: a built-in type, then an alias, then a
 * declared one. False when nothing of that name has been declared. */
bool csTypeLookupName(const TypeTable *table, const char *name, int length, TypeId *out);

/* The member a name refers to, following nothing: an interface's members are
 * all of them, because `extends` copies them in. */
const TypeMember *csTypeFindMember(const TypeTable *table, TypeId type, const char *name, int length);

/* Names any type, including a composite — `number[]`, `(a: number) => string`,
 * `Point`, `string | null`.
 *
 * The answer lives in a small rotating set of buffers owned by this module, so
 * that one message may name several types. A name is valid until a few more
 * have been asked for; nothing here keeps one. */
const char *csTypeNameIn(const TypeTable *table, TypeId type);

/* Like csTypeAssignable, and additionally structural where a composite is
 * involved: one interface satisfies another when it has every member the other
 * requires; an array is assignable when its elements are; a function when it
 * takes no more than it is given and answers no less. */
bool csTypeAssignableIn(const TypeTable *table, TypeId from, TypeId to);

/* Re-interns a type declared in one file's table into another's, so that what
 * a module exports can be named by whatever imports it.
 *
 * Structural all the way down, which is what makes this possible at all: two
 * files describing the same shape end up with the same type without either
 * knowing the other. A type variable is erased to DYNAMIC on the way across —
 * a generic's instantiation happens at its call site, and an imported binding
 * has no call site the checker can see. */
TypeId csTypeImport(TypeTable *dest, const TypeTable *src, TypeId type);

/* --- the built-in generics ----------------------------------------------- */

/* Declares `Map<K, V>`, `Set<T>` and `Promise<T>` into a fresh table, so that
 * they resolve exactly as a file's own `interface` does. Called once per
 * table, before anything the file itself declares.
 *
 * `Array<T>` is not among them: it is a spelling of `T[]`, turned into one by
 * the parser so the two can never become two types. */
void csTypeDeclarePrelude(TypeTable *table);

/* True when the type is a Promise however it was instantiated, and answers
 * what it resolves to — which is what `await` unwraps. */
bool csTypeIsPromise(const TypeTable *table, TypeId type, TypeId *resolves);

/* --- generics ------------------------------------------------------------ */

/* Declares `T` inside the declaration that introduced it. In scope until the
 * parser closes it with csTypeCloseTypeVar. */
TypeId csTypeDeclareTypeVar(TypeTable *table, const char *name, int length);
void csTypeCloseTypeVar(TypeTable *table, TypeId typeVar);

/* Records the type parameters a declaration introduced, so a call or a type
 * reference can instantiate it. */
void csTypeSetTypeParams(TypeTable *table, TypeId type, const TypeId *params, int count);
int csTypeTypeParamCount(const TypeTable *table, TypeId type);

/* `Box<number>` — substitutes the arguments for the declaration's own type
 * parameters, throughout. Answers `generic` unchanged when it takes none. */
TypeId csTypeInstantiate(TypeTable *table, TypeId generic, const TypeId *args, int argCount);

/* Substitutes `args` for `params` anywhere inside `type`. The engine under
 * instantiation, and under inference at a call site. */
TypeId csTypeSubstitute(TypeTable *table, TypeId type, const TypeId *params, const TypeId *args, int count);

/* Matches an argument's type against a parameter's, recording what each type
 * variable must be. `bindings` is indexed the same as `params`, and starts as
 * TYPE_DYNAMIC for "not yet known". */
void csTypeInfer(const TypeTable *table, TypeId parameter, TypeId argument, const TypeId *params, TypeId *bindings, int count);

/* True when the type mentions a type variable anywhere inside it. */
bool csTypeMentionsTypeVar(const TypeTable *table, TypeId type);

#endif /* CSCRIPT_TYPE_H */
