/* type_prelude.c — the generic types every file starts with.
 *
 * `Map<K, V>`, `Set<T>` and `Promise<T>` are built into the runtime, and this
 * is where the checker learns their shapes. They are declared into each file's
 * table the moment it is created, so they resolve exactly as a file's own
 * `interface` does and instantiate through exactly the same machinery — there
 * is no second kind of generic and no special case in the parser.
 *
 * `Array<T>` is not here: it is a *spelling* of `T[]`, and the parser turns one
 * into the other so that the two can never become two types.
 *
 * What each shape carries is what a program actually asks of it. A method that
 * answers the receiver — `set` on a Map — answers the instantiation rather than
 * `this`, which this lattice has no word for, and the chain still checks.
 *
 * The type variables are closed as soon as the shape is built. Nothing in a
 * program should resolve the name `K` to a Map's key, and substitution works on
 * ids rather than on names, so closing them costs nothing.
 */
#include <string.h>

#include "compiler/type_internal.h"

/* A member holding a function, spelled out at the call sites below. */
static void addMethod(TypeTable *table, TypeId owner, const char *name, const TypeId *params, int paramCount, int requiredCount, TypeId result) {
  TypeMember member;
  memset(&member, 0, sizeof member);
  member.name = name;
  member.length = (int)strlen(name);
  member.type = csTypeFunctionOf(table, params, paramCount, requiredCount, false, result);
  member.optional = false;
  csTypeAddMember(table, owner, &member);
}

static void addField(TypeTable *table, TypeId owner, const char *name, TypeId type) {
  TypeMember member;
  memset(&member, 0, sizeof member);
  member.name = name;
  member.length = (int)strlen(name);
  member.type = type;
  member.optional = false;
  csTypeAddMember(table, owner, &member);
}

/* `Map<K, V>` — get answers `V | undefined`, because a key that is not there
 * is the case the caller has to handle and the one a type can say out loud. */
static void declareMap(TypeTable *table) {
  TypeId key = csTypeDeclareTypeVar(table, "K", 1);
  TypeId held = csTypeDeclareTypeVar(table, "V", 1);
  if (key == TYPE_ERROR || held == TYPE_ERROR) return;

  TypeId map = csTypeDeclareInterface(table, "Map", 3);
  if (map == TYPE_ERROR) return;
  TypeId params[2] = {key, held};
  csTypeSetTypeParams(table, map, params, 2);

  TypeId one[1] = {key};
  TypeId two[2] = {key, held};
  TypeId callback[1] = {csTypeFunctionOf(table, two, 2, 2, false, TYPE_DYNAMIC)};

  addField(table, map, "size", TYPE_NUMBER);
  addMethod(table, map, "get", one, 1, 1, csTypeUnionWith(table, held, TYPE_UNDEFINED));
  addMethod(table, map, "set", two, 2, 2, map);
  addMethod(table, map, "has", one, 1, 1, TYPE_BOOLEAN);
  addMethod(table, map, "delete", one, 1, 1, TYPE_BOOLEAN);
  addMethod(table, map, "clear", NULL, 0, 0, TYPE_UNDEFINED);
  addMethod(table, map, "keys", NULL, 0, 0, csTypeArrayOf(table, key));
  addMethod(table, map, "values", NULL, 0, 0, csTypeArrayOf(table, held));
  addMethod(table, map, "entries", NULL, 0, 0, TYPE_ARRAY);
  addMethod(table, map, "forEach", callback, 1, 1, TYPE_UNDEFINED);

  csTypeCloseTypeVar(table, key);
  csTypeCloseTypeVar(table, held);
}

static void declareSet(TypeTable *table) {
  TypeId element = csTypeDeclareTypeVar(table, "T", 1);
  if (element == TYPE_ERROR) return;

  TypeId set = csTypeDeclareInterface(table, "Set", 3);
  if (set == TYPE_ERROR) return;
  csTypeSetTypeParams(table, set, &element, 1);

  TypeId one[1] = {element};
  TypeId callback[1] = {csTypeFunctionOf(table, one, 1, 1, false, TYPE_DYNAMIC)};

  addField(table, set, "size", TYPE_NUMBER);
  addMethod(table, set, "add", one, 1, 1, set);
  addMethod(table, set, "has", one, 1, 1, TYPE_BOOLEAN);
  addMethod(table, set, "delete", one, 1, 1, TYPE_BOOLEAN);
  addMethod(table, set, "clear", NULL, 0, 0, TYPE_UNDEFINED);
  addMethod(table, set, "values", NULL, 0, 0, csTypeArrayOf(table, element));
  addMethod(table, set, "keys", NULL, 0, 0, csTypeArrayOf(table, element));
  addMethod(table, set, "entries", NULL, 0, 0, TYPE_ARRAY);
  addMethod(table, set, "forEach", callback, 1, 1, TYPE_UNDEFINED);

  csTypeCloseTypeVar(table, element);
}

/* `Promise<T>` — what `await` unwraps, and what an async function answers.
 *
 * `then` is typed loosely on purpose: its callback may answer another promise,
 * and flattening that is a rule this lattice cannot express. `await` is the
 * way to read one exactly, and is what a program should reach for. */
static void declarePromise(TypeTable *table) {
  TypeId held = csTypeDeclareTypeVar(table, "T", 1);
  if (held == TYPE_ERROR) return;

  TypeId promise = csTypeDeclareInterface(table, "Promise", 7);
  if (promise == TYPE_ERROR) return;
  csTypeSetTypeParams(table, promise, &held, 1);

  /* `then(onValue, onRejection)` — both optional, as JavaScript has them, and
   * the second is why `p.then(ok, fail)` must not look like one argument too
   * many. */
  TypeId handlers[2] = {csTypeFunctionOf(table, &held, 1, 1, false, TYPE_DYNAMIC), TYPE_FUNCTION};
  TypeId onError[1] = {TYPE_FUNCTION};

  addMethod(table, promise, "then", handlers, 2, 0, TYPE_DYNAMIC);
  addMethod(table, promise, "catch", onError, 1, 0, TYPE_DYNAMIC);
  addMethod(table, promise, "finally", onError, 1, 0, TYPE_DYNAMIC);

  csTypeCloseTypeVar(table, held);
}

void csTypeDeclarePrelude(TypeTable *table) {
  declareMap(table);
  declareSet(table);
  declarePromise(table);
}

/* The generic behind an instantiation — `Map<string, number>` answers the
 * `Map<K, V>` it came from, and a `Map<K, V>` answers itself. TYPE_ERROR for
 * anything that is not one, which is what the callers test against. */
static TypeId genericBehind(const TypeTable *table, TypeId type) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL || composite->kind != COMPOSITE_INTERFACE) return TYPE_ERROR;
  return composite->genericOf != TYPE_DYNAMIC ? composite->genericOf : type;
}

/* True when `type` is a Promise, however it was instantiated, and answers what
 * it resolves to. The prelude is per file, so the question is asked by name:
 * two files' Promises are different ids and the same type. */
bool csTypeIsPromise(const TypeTable *table, TypeId type, TypeId *resolves) {
  TypeId generic = genericBehind(table, type);
  const CompositeType *shape = csTypeComposite(table, generic);
  if (shape == NULL || !csTypeNameMatches(shape->name, shape->nameLength, "Promise")) return false;

  const CompositeType *instance = csTypeComposite(table, type);
  *resolves = instance->slotCount > 0 ? table->slots[instance->slotStart] : TYPE_DYNAMIC;
  return true;
}
