/* type_table.c — the table the composite types live in, and how one is named.
 *
 * A composite type is an entry here and an index into it. Entries are
 * **interned**: asking for `number[]` twice answers the same id both times, so
 * two types are usually equal when their ids are — and structural equality
 * falls out of construction rather than being computed at every comparison.
 *
 * The pools are the reason the entries are small. Members and slots live in
 * two flat arrays, and an entry holds a start and a count into them, so an
 * interface with two members costs two member slots rather than a fixed
 * maximum. What it costs instead is that an interface has to be built in one
 * go, while its run is the last one in the pool — which is exactly how the
 * parser builds one.
 */
#include <stdio.h>
#include <string.h>

#include "compiler/type_internal.h"

/* --- the table ----------------------------------------------------------- */

void csTypeTableInit(TypeTable *table) {
  table->compositeCount = 0;
  table->memberCount = 0;
  table->slotCount = 0;
  table->aliasCount = 0;
  table->full = false;
}

const CompositeType *csTypeComposite(const TypeTable *table, TypeId type) {
  if (table == NULL || !csTypeIsComposite(type)) return NULL;
  int index = csTypeCompositeIndex(type);
  if (index >= table->compositeCount) return NULL;
  return &table->composites[index];
}

bool csTypeIs(const TypeTable *table, TypeId type, CompositeKind kind) {
  const CompositeType *composite = csTypeComposite(table, type);
  return composite != NULL && composite->kind == kind;
}

/* A blank composite of the given kind, or NULL when the table is full. */
static CompositeType *newComposite(TypeTable *table, CompositeKind kind, TypeId *idOut) {
  if (table == NULL || table->compositeCount >= CS_MAX_COMPOSITE_TYPES) {
    if (table != NULL) table->full = true;
    return NULL;
  }
  CompositeType *composite = &table->composites[table->compositeCount];
  memset(composite, 0, sizeof *composite);
  composite->kind = kind;
  composite->inner = TYPE_DYNAMIC;
  composite->genericOf = TYPE_DYNAMIC;
  *idOut = csTypeCompositeAt(table->compositeCount++);
  return composite;
}

/* Reserves a run of the slot pool and fills it, or answers false when full. */
static bool takeSlots(TypeTable *table, const TypeId *values, int count, int *startOut) {
  if (table->slotCount + count > CS_MAX_TYPE_SLOTS) {
    table->full = true;
    return false;
  }
  *startOut = table->slotCount;
  for (int i = 0; i < count; i++) table->slots[table->slotCount++] = values[i];
  return true;
}

static bool sameSlots(const TypeTable *table, const CompositeType *composite, const TypeId *values, int count) {
  if (composite->slotCount != count) return false;
  for (int i = 0; i < count; i++) {
    if (table->slots[composite->slotStart + i] != values[i]) return false;
  }
  return true;
}

TypeId csTypeArrayOf(TypeTable *table, TypeId element) {
  if (table == NULL) return TYPE_ARRAY;
  /* An array of nothing in particular is the primitive `array`, so the two
   * spellings do not become two types. */
  if (element == TYPE_DYNAMIC) return TYPE_ARRAY;

  for (int i = 0; i < table->compositeCount; i++) {
    const CompositeType *composite = &table->composites[i];
    if (composite->kind == COMPOSITE_ARRAY && composite->inner == element) return csTypeCompositeAt(i);
  }

  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_ARRAY, &id);
  if (composite == NULL) return TYPE_ARRAY;
  composite->inner = element;
  return id;
}

TypeId csTypeFunctionOf(TypeTable *table, const TypeId *params, int paramCount, int requiredCount, bool hasRest, TypeId result) {
  if (table == NULL) return TYPE_FUNCTION;

  for (int i = 0; i < table->compositeCount; i++) {
    const CompositeType *composite = &table->composites[i];
    if (composite->kind != COMPOSITE_FUNCTION) continue;
    if (composite->inner != result || composite->requiredCount != requiredCount || composite->hasRest != hasRest) continue;
    if (!sameSlots(table, composite, params, paramCount)) continue;
    return csTypeCompositeAt(i);
  }

  int start;
  if (!takeSlots(table, params, paramCount, &start)) return TYPE_FUNCTION;

  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_FUNCTION, &id);
  if (composite == NULL) return TYPE_FUNCTION;
  composite->slotStart = start;
  composite->slotCount = paramCount;
  composite->requiredCount = requiredCount;
  composite->hasRest = hasRest;
  composite->inner = result;
  return id;
}

/* Members are kept in ascending id order, so that `A | B` and `B | A` are one
 * type — which is what makes a union comparable by id like everything else. */
TypeId csTypeUnionOf(TypeTable *table, const TypeId *members, int count) {
  if (table == NULL || count <= 0) return TYPE_DYNAMIC;

  TypeId flat[16];
  int flatCount = 0;
  for (int i = 0; i < count; i++) {
    TypeId member = members[i];

    /* The absorbing answers: what the checker could not see swallows the
     * union, and so does the top type — `unknown | string` is `unknown`,
     * because it still promises nothing. */
    if (member == TYPE_DYNAMIC) return TYPE_DYNAMIC;
    if (member == TYPE_UNKNOWN) return TYPE_UNKNOWN;
    if (member == TYPE_ERROR) return TYPE_ERROR;

    /* A union of unions is one union. */
    const CompositeType *nested = csTypeComposite(table, member);
    bool isUnion = nested != NULL && nested->kind == COMPOSITE_UNION;
    int nestedCount = isUnion ? nested->slotCount : 1;
    for (int j = 0; j < nestedCount; j++) {
      TypeId one = isUnion ? table->slots[nested->slotStart + j] : member;
      bool already = false;
      for (int k = 0; k < flatCount && !already; k++) already = flat[k] == one;
      if (already) continue;
      if (flatCount >= (int)(sizeof flat / sizeof flat[0])) return TYPE_DYNAMIC;

      int at = flatCount++;
      while (at > 0 && flat[at - 1] > one) {
        flat[at] = flat[at - 1];
        at--;
      }
      flat[at] = one;
    }
  }

  if (flatCount == 1) return flat[0];

  for (int i = 0; i < table->compositeCount; i++) {
    const CompositeType *composite = &table->composites[i];
    if (composite->kind != COMPOSITE_UNION) continue;
    if (!sameSlots(table, composite, flat, flatCount)) continue;
    return csTypeCompositeAt(i);
  }

  int start;
  if (!takeSlots(table, flat, flatCount, &start)) return TYPE_DYNAMIC;

  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_UNION, &id);
  if (composite == NULL) return TYPE_DYNAMIC;
  composite->slotStart = start;
  composite->slotCount = flatCount;
  return id;
}

TypeId csTypeUnionWith(TypeTable *table, TypeId left, TypeId right) {
  TypeId both[2] = {left, right};
  return csTypeUnionOf(table, both, 2);
}

TypeId csTypeElementOf(const TypeTable *table, TypeId array) {
  const CompositeType *composite = csTypeComposite(table, array);
  if (composite != NULL && composite->kind == COMPOSITE_ARRAY) return composite->inner;
  return TYPE_DYNAMIC;
}

bool csTypeIsArrayLike(const TypeTable *table, TypeId type) {
  return type == TYPE_ARRAY || csTypeIs(table, type, COMPOSITE_ARRAY);
}

bool csTypeIsCallable(const TypeTable *table, TypeId type) {
  return type == TYPE_FUNCTION || csTypeIs(table, type, COMPOSITE_FUNCTION);
}

/* --- declared types ------------------------------------------------------ */

TypeId csTypeDeclareInterface(TypeTable *table, const char *name, int length) {
  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_INTERFACE, &id);
  if (composite == NULL) return TYPE_ERROR;
  composite->name = name;
  composite->nameLength = length;
  composite->memberStart = table->memberCount;
  composite->memberCount = 0;
  return id;
}

TypeId csTypeDeclareClass(TypeTable *table, const char *name, int length) {
  TypeId id = csTypeDeclareInterface(table, name, length);
  if (id == TYPE_ERROR) return id;
  table->composites[csTypeCompositeIndex(id)].open = true;
  return id;
}

bool csTypeIsOpen(const TypeTable *table, TypeId type) {
  const CompositeType *composite = csTypeComposite(table, type);
  return composite != NULL && composite->open;
}

TypeId csTypeDeclareTypeVar(TypeTable *table, const char *name, int length) {
  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_TYPEVAR, &id);
  if (composite == NULL) return TYPE_ERROR;
  composite->name = name;
  composite->nameLength = length;
  composite->active = true;
  return id;
}

void csTypeCloseTypeVar(TypeTable *table, TypeId typeVar) {
  if (csTypeComposite(table, typeVar) == NULL) return;
  table->composites[csTypeCompositeIndex(typeVar)].active = false;
}

void csTypeSetTypeParams(TypeTable *table, TypeId type, const TypeId *params, int count) {
  if (csTypeComposite(table, type) == NULL) return;
  CompositeType *composite = &table->composites[csTypeCompositeIndex(type)];
  if (count > CS_MAX_TYPE_PARAMS) count = CS_MAX_TYPE_PARAMS;
  for (int i = 0; i < count; i++) composite->typeParams[i] = params[i];
  composite->typeParamCount = count;
}

int csTypeTypeParamCount(const TypeTable *table, TypeId type) {
  const CompositeType *composite = csTypeComposite(table, type);
  return composite != NULL ? composite->typeParamCount : 0;
}

/* Members are appended to the pool, so an interface's run has to be the last
 * one while it is being built. That is exactly how the parser builds one —
 * name, then members, then done — and it is why an interface cannot be
 * reopened afterwards. */
bool csTypeAddMember(TypeTable *table, TypeId type, const TypeMember *member) {
  const CompositeType *found = csTypeComposite(table, type);
  if (found == NULL || found->kind != COMPOSITE_INTERFACE) return false;
  if (table->memberCount >= CS_MAX_TYPE_MEMBERS) {
    table->full = true;
    return false;
  }

  CompositeType *declared = &table->composites[csTypeCompositeIndex(type)];
  if (declared->memberStart + declared->memberCount != table->memberCount) return false;
  for (int i = 0; i < declared->memberCount; i++) {
    if (csTypeNameMatches(member->name, member->length, table->members[declared->memberStart + i].name)) return false;
  }
  table->members[table->memberCount++] = *member;
  declared->memberCount++;
  return true;
}

bool csTypeDeclareAlias(TypeTable *table, const char *name, int length, TypeId type) {
  if (table == NULL || table->aliasCount >= CS_MAX_TYPE_ALIASES) return false;
  table->aliases[table->aliasCount].name = name;
  table->aliases[table->aliasCount].length = length;
  table->aliases[table->aliasCount].type = type;
  table->aliasCount++;
  return true;
}

bool csTypeLookupName(const TypeTable *table, const char *name, int length, TypeId *out) {
  if (csTypeFromName(name, length, out)) return true;
  if (table == NULL) return false;

  /* An alias is a second name for a type that already exists, so it is looked
   * up before the declarations: `type Point2 = Point` records Point's own type
   * rather than a copy of it. */
  for (int i = 0; i < table->aliasCount; i++) {
    if (!csTypeNameMatches(name, length, table->aliases[i].name)) continue;
    *out = table->aliases[i].type;
    return true;
  }
  /* Backwards, so that a type variable shadows anything of the same name for
   * as long as the declaration that introduced it is being parsed. */
  for (int i = table->compositeCount - 1; i >= 0; i--) {
    const CompositeType *composite = &table->composites[i];
    if (composite->name == NULL) continue;
    if (composite->kind != COMPOSITE_INTERFACE && composite->kind != COMPOSITE_TYPEVAR) continue;
    if (composite->kind == COMPOSITE_TYPEVAR && !composite->active) continue;
    /* `Box<number>` carries the name `Box` for its messages, but the name in
     * a program still means the declaration it came from — otherwise the
     * first instantiation would shadow the generic and `Box<string>` would
     * become impossible to write. */
    if (composite->genericOf != TYPE_DYNAMIC) continue;
    if (!csTypeNameMatches(name, length, composite->name)) continue;
    *out = csTypeCompositeAt(i);
    return true;
  }
  return false;
}

const TypeMember *csTypeFindMember(const TypeTable *table, TypeId type, const char *name, int length) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL || composite->kind != COMPOSITE_INTERFACE) return NULL;
  for (int i = 0; i < composite->memberCount; i++) {
    const TypeMember *member = &table->members[composite->memberStart + i];
    if (csTypeNameMatches(name, length, member->name)) return member;
  }
  return NULL;
}

/* --- naming -------------------------------------------------------------- */

/* Written into the caller's buffer rather than returned, so a nested type
 * needs no buffer of its own. Answers how much was written. */
static int renderType(const TypeTable *table, TypeId type, char *out, int cap, int depth) {
  if (cap <= 1) return 0;
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL) return snprintf(out, (size_t)cap, "%s", csTypeName(type));

  /* A type that refers to itself — `interface Link { next?: Link }` — is named
   * rather than followed, which is what a reader would do too. */
  if (depth > 4) return snprintf(out, (size_t)cap, "...");

  int written = 0;
  switch (composite->kind) {
    case COMPOSITE_INTERFACE:
    case COMPOSITE_TYPEVAR:
      written = snprintf(out, (size_t)cap, "%.*s", composite->nameLength, composite->name);
      if (composite->slotCount == 0) return written;

      /* An instantiation says what it was given: `Box<number>`. */
      written += snprintf(out + written, (size_t)(cap - written), "<");
      for (int i = 0; i < composite->slotCount && written < cap - 1; i++) {
        if (i > 0) written += snprintf(out + written, (size_t)(cap - written), ", ");
        written += renderType(table, table->slots[composite->slotStart + i], out + written, cap - written, depth + 1);
      }
      return written + snprintf(out + written, (size_t)(cap - written), ">");

    case COMPOSITE_ARRAY: {
      /* `(string | null)[]` — without the parentheses that reads as a union
       * with an array in it, which is a different type. */
      bool wrap = csTypeIs(table, composite->inner, COMPOSITE_UNION) || csTypeIs(table, composite->inner, COMPOSITE_FUNCTION);
      if (wrap) written += snprintf(out, (size_t)cap, "(");
      written += renderType(table, composite->inner, out + written, cap - written, depth + 1);
      if (wrap) written += snprintf(out + written, (size_t)(cap - written), ")");
      return written + snprintf(out + written, (size_t)(cap - written), "[]");
    }

    case COMPOSITE_FUNCTION:
      written = snprintf(out, (size_t)cap, "(");
      for (int i = 0; i < composite->slotCount && written < cap - 1; i++) {
        if (i > 0) written += snprintf(out + written, (size_t)(cap - written), ", ");
        if (composite->hasRest && i == composite->slotCount - 1) written += snprintf(out + written, (size_t)(cap - written), "...");
        written += renderType(table, table->slots[composite->slotStart + i], out + written, cap - written, depth + 1);
      }
      written += snprintf(out + written, (size_t)(cap - written), ") => ");
      return written + renderType(table, composite->inner, out + written, cap - written, depth + 1);

    case COMPOSITE_UNION:
      for (int i = 0; i < composite->slotCount && written < cap - 1; i++) {
        if (i > 0) written += snprintf(out + written, (size_t)(cap - written), " | ");
        written += renderType(table, table->slots[composite->slotStart + i], out + written, cap - written, depth + 1);
      }
      return written;
  }
  return snprintf(out, (size_t)cap, "%s", csTypeName(type));
}

const char *csTypeNameIn(const TypeTable *table, TypeId type) {
  if (!csTypeIsComposite(type)) return csTypeName(type);

  /* One message may name several types — "argument 1 is X but the parameter is
   * Y" — so the answers rotate rather than sharing one buffer. Nothing keeps a
   * name, and a message uses at most a few. */
  enum { BUFFERS = 6, WIDTH = 160 };
  static char buffers[BUFFERS][WIDTH];
  static int next = 0;

  char *out = buffers[next];
  next = (next + 1) % BUFFERS;
  renderType(table, type, out, WIDTH, 0);
  return out;
}

/* --- generics ------------------------------------------------------------ */

static bool mentions(const TypeTable *table, TypeId type, int depth) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL || depth > 6) return false;
  if (composite->kind == COMPOSITE_TYPEVAR) return true;
  if (mentions(table, composite->inner, depth + 1)) return true;
  for (int i = 0; i < composite->slotCount; i++) {
    if (mentions(table, table->slots[composite->slotStart + i], depth + 1)) return true;
  }
  for (int i = 0; i < composite->memberCount; i++) {
    if (mentions(table, table->members[composite->memberStart + i].type, depth + 1)) return true;
  }
  return false;
}

bool csTypeMentionsTypeVar(const TypeTable *table, TypeId type) {
  return mentions(table, type, 0);
}

static TypeId substitute(TypeTable *table, TypeId type, const TypeId *params, const TypeId *args, int count, int depth) {
  if (depth > 6 || !csTypeIsComposite(type)) return type;
  for (int i = 0; i < count; i++) {
    if (type == params[i]) return args[i];
  }
  if (!mentions(table, type, 0)) return type;

  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL) return type;

  switch (composite->kind) {
    case COMPOSITE_TYPEVAR: return type;

    case COMPOSITE_ARRAY: return csTypeArrayOf(table, substitute(table, composite->inner, params, args, count, depth + 1));

    case COMPOSITE_UNION: {
      TypeId members[16];
      int memberCount = composite->slotCount;
      if (memberCount > (int)(sizeof members / sizeof members[0])) return type;
      for (int i = 0; i < memberCount; i++) {
        members[i] = substitute(table, table->slots[composite->slotStart + i], params, args, count, depth + 1);
      }
      return csTypeUnionOf(table, members, memberCount);
    }

    case COMPOSITE_FUNCTION: {
      TypeId slots[16];
      int slotCount = composite->slotCount;
      if (slotCount > (int)(sizeof slots / sizeof slots[0])) return type;
      for (int i = 0; i < slotCount; i++) {
        slots[i] = substitute(table, table->slots[composite->slotStart + i], params, args, count, depth + 1);
      }
      TypeId result = substitute(table, composite->inner, params, args, count, depth + 1);
      return csTypeFunctionOf(table, slots, slotCount, composite->requiredCount, composite->hasRest, result);
    }

    case COMPOSITE_INTERFACE: {
      /* A new shape with the arguments put through it, recorded as an
       * instantiation of the one it came from so that a message can say
       * `Box<number>` rather than a second, unexplained `Box`. */
      int sourceStart = composite->memberStart;
      int sourceCount = composite->memberCount;

      TypeId copy = csTypeDeclareInterface(table, composite->name, composite->nameLength);
      if (copy == TYPE_ERROR) return type;

      for (int i = 0; i < sourceCount; i++) {
        TypeMember member = table->members[sourceStart + i];
        member.type = substitute(table, member.type, params, args, count, depth + 1);
        if (!csTypeAddMember(table, copy, &member)) break;
      }

      int start;
      CompositeType *made = &table->composites[csTypeCompositeIndex(copy)];
      if (takeSlots(table, args, count, &start)) {
        made->slotStart = start;
        made->slotCount = count;
      }
      made->genericOf = type;
      return copy;
    }
  }
  return type;
}

TypeId csTypeSubstitute(TypeTable *table, TypeId type, const TypeId *params, const TypeId *args, int count) {
  if (count <= 0) return type;
  return substitute(table, type, params, args, count, 0);
}

TypeId csTypeInstantiate(TypeTable *table, TypeId generic, const TypeId *args, int argCount) {
  const CompositeType *composite = csTypeComposite(table, generic);
  if (composite == NULL || composite->typeParamCount == 0) return generic;

  TypeId params[CS_MAX_TYPE_PARAMS];
  TypeId filled[CS_MAX_TYPE_PARAMS];
  int count = composite->typeParamCount;
  for (int i = 0; i < count; i++) {
    params[i] = composite->typeParams[i];
    /* A missing argument is one the caller did not say and the checker could
     * not work out, which is exactly DYNAMIC. */
    filled[i] = i < argCount ? args[i] : TYPE_DYNAMIC;
  }
  return csTypeSubstitute(table, generic, params, filled, count);
}

/* Matching is structural and one-way: it walks the parameter's type and the
 * argument's together, and every time the parameter is a variable, the
 * argument says what that variable must be. The first answer wins — a later,
 * different one is a mismatch the ordinary check reports. */
static void infer(const TypeTable *table, TypeId parameter, TypeId argument, const TypeId *params, TypeId *bindings, int count, int depth) {
  if (depth > 6 || argument == TYPE_ERROR) return;

  for (int i = 0; i < count; i++) {
    if (parameter != params[i]) continue;
    if (bindings[i] == TYPE_DYNAMIC) bindings[i] = argument;
    return;
  }

  const CompositeType *want = csTypeComposite(table, parameter);
  const CompositeType *given = csTypeComposite(table, argument);
  if (want == NULL) return;

  if (want->kind == COMPOSITE_ARRAY) {
    /* `T[]` against `number[]` says T is a number; against a bare `array` it
     * says nothing, which leaves T to another argument or to DYNAMIC. */
    if (given != NULL && given->kind == COMPOSITE_ARRAY) infer(table, want->inner, given->inner, params, bindings, count, depth + 1);
    return;
  }

  if (want->kind == COMPOSITE_FUNCTION && given != NULL && given->kind == COMPOSITE_FUNCTION) {
    int shared = want->slotCount < given->slotCount ? want->slotCount : given->slotCount;
    for (int i = 0; i < shared; i++) {
      infer(table, table->slots[want->slotStart + i], table->slots[given->slotStart + i], params, bindings, count, depth + 1);
    }
    infer(table, want->inner, given->inner, params, bindings, count, depth + 1);
    return;
  }

  if (want->kind == COMPOSITE_INTERFACE && given != NULL && given->kind == COMPOSITE_INTERFACE) {
    for (int i = 0; i < want->memberCount; i++) {
      const TypeMember *member = &table->members[want->memberStart + i];
      const TypeMember *match = csTypeFindMember(table, argument, member->name, member->length);
      if (match == NULL) continue;
      infer(table, member->type, match->type, params, bindings, count, depth + 1);
    }
  }
}

void csTypeInfer(const TypeTable *table, TypeId parameter, TypeId argument, const TypeId *params, TypeId *bindings, int count) {
  infer(table, parameter, argument, params, bindings, count, 0);
}
