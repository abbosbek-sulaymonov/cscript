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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "compiler/type_internal.h"

/* --- the table ----------------------------------------------------------- */

void csTypeTableInit(TypeTable *table) {
  table->compositeCount = 0;
  table->nameCount = 0;
  table->memberCount = 0;
  table->slotCount = 0;
  table->aliasCount = 0;
  table->full = false;

  /* Every file starts knowing the shapes the runtime already has. */
  csTypeDeclarePrelude(table);
}

/* A name the table owns. Repeats share one copy, which keeps the pool small
 * enough to be a fixed size: a file's type names are mostly its members', and
 * every shape that has an `x` wants the same three bytes. */
static const char *internName(TypeTable *table, const char *name, int length) {
  if (name == NULL) return NULL;
  for (int i = 0; i + length < table->nameCount; i++) {
    if (table->names[i + length] != '\0') continue;
    if (i > 0 && table->names[i - 1] != '\0') continue;
    if (memcmp(&table->names[i], name, (size_t)length) == 0) return &table->names[i];
  }
  if (table->nameCount + length + 1 > CS_MAX_TYPE_NAME_CHARS) {
    table->full = true;
    return NULL;
  }
  char *copy = &table->names[table->nameCount];
  memcpy(copy, name, (size_t)length);
  copy[length] = '\0';
  table->nameCount += length + 1;
  return copy;
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

bool csTypeTakeSlots(TypeTable *table, const TypeId *values, int count, int *startOut) {
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
  if (!csTypeTakeSlots(table, params, paramCount, &start)) return TYPE_FUNCTION;

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
  if (!csTypeTakeSlots(table, flat, flatCount, &start)) return TYPE_DYNAMIC;

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
  if (type == TYPE_FUNCTION || csTypeIs(table, type, COMPOSITE_FUNCTION)) return true;

  /* `order === null ? compare : order` answers one function or another, and
   * calling it is exactly what the caller then does. A union is callable when
   * every member is. */
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL || composite->kind != COMPOSITE_UNION) return false;
  for (int i = 0; i < composite->slotCount; i++) {
    if (!csTypeIsCallable(table, table->slots[composite->slotStart + i])) return false;
  }
  return true;
}

TypeId csTypeResultOf(TypeTable *table, TypeId callable) {
  const CompositeType *composite = csTypeComposite(table, callable);
  if (composite == NULL) return TYPE_DYNAMIC;
  if (composite->kind == COMPOSITE_FUNCTION) return composite->inner;
  if (composite->kind != COMPOSITE_UNION) return TYPE_DYNAMIC;

  TypeId results[16];
  int count = composite->slotCount;
  if (count > (int)(sizeof results / sizeof results[0])) return TYPE_DYNAMIC;
  for (int i = 0; i < count; i++) results[i] = csTypeResultOf(table, table->slots[composite->slotStart + i]);
  return csTypeUnionOf(table, results, count);
}

/* --- declared types ------------------------------------------------------ */

TypeId csTypeDeclareInterface(TypeTable *table, const char *name, int length) {
  const char *owned = internName(table, name, length);
  if (owned == NULL) return TYPE_ERROR;

  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_INTERFACE, &id);
  if (composite == NULL) return TYPE_ERROR;
  composite->name = owned;
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
  const char *owned = internName(table, name, length);
  if (owned == NULL) return TYPE_ERROR;

  TypeId id;
  CompositeType *composite = newComposite(table, COMPOSITE_TYPEVAR, &id);
  if (composite == NULL) return TYPE_ERROR;
  composite->name = owned;
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

  /* Members live in one pool and an interface owns a run of it, so a member
   * can only be appended while that run is the last. Instantiating a generic
   * whose members mention another generic interleaves two of them, so the run
   * moves to the end rather than refusing — which costs a copy of a handful of
   * entries, once, and removes an edge nothing else would have noticed. */
  if (declared->memberStart + declared->memberCount != table->memberCount) {
    if (table->memberCount + declared->memberCount >= CS_MAX_TYPE_MEMBERS) {
      table->full = true;
      return false;
    }
    for (int i = 0; i < declared->memberCount; i++) {
      table->members[table->memberCount + i] = table->members[declared->memberStart + i];
    }
    declared->memberStart = table->memberCount;
    table->memberCount += declared->memberCount;
  }
  for (int i = 0; i < declared->memberCount; i++) {
    if (csTypeNameMatches(member->name, member->length, table->members[declared->memberStart + i].name)) return false;
  }
  table->members[table->memberCount] = *member;
  table->members[table->memberCount].name = internName(table, member->name, member->length);
  if (table->members[table->memberCount].name == NULL) return false;
  table->memberCount++;
  declared->memberCount++;
  return true;
}

bool csTypeDeclareAlias(TypeTable *table, const char *name, int length, TypeId type) {
  if (table == NULL || table->aliasCount >= CS_MAX_TYPE_ALIASES) return false;
  table->aliases[table->aliasCount].name = internName(table, name, length);
  if (table->aliases[table->aliasCount].name == NULL) return false;
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

/* Where a name is being built, and how much room is left.
 *
 * Every piece goes through `append`, which clamps rather than trusting
 * snprintf's return: that return is what the text *would* have taken, so
 * adding it to a length and using the difference as a size is how a buffer
 * overruns and how a name comes back with a newline in it. */
typedef struct {
  char *out;
  int cap;
  int length;
} NameBuffer;

static void append(NameBuffer *buffer, const char *format, ...) {
  int room = buffer->cap - buffer->length;
  if (room <= 1) return;

  va_list args;
  va_start(args, format);
  int written = vsnprintf(buffer->out + buffer->length, (size_t)room, format, args);
  va_end(args);
  if (written < 0) return;
  buffer->length += written < room ? written : room - 1;
}

static void renderType(const TypeTable *table, TypeId type, NameBuffer *buffer, int depth) {
  const CompositeType *composite = csTypeComposite(table, type);
  if (composite == NULL) {
    append(buffer, "%s", csTypeName(type));
    return;
  }

  /* A type that refers to itself — `interface Link { next?: Link }` — is named
   * rather than followed, which is what a reader would do too. */
  if (depth > 4) {
    append(buffer, "...");
    return;
  }

  switch (composite->kind) {
    case COMPOSITE_INTERFACE:
    case COMPOSITE_TYPEVAR:
      append(buffer, "%.*s", composite->nameLength, composite->name);
      if (composite->slotCount == 0) return;

      /* An instantiation says what it was given: `Box<number>`. */
      append(buffer, "<");
      for (int i = 0; i < composite->slotCount; i++) {
        if (i > 0) append(buffer, ", ");
        renderType(table, table->slots[composite->slotStart + i], buffer, depth + 1);
      }
      append(buffer, ">");
      return;

    case COMPOSITE_ARRAY: {
      /* `(string | null)[]` — without the parentheses that reads as a union
       * with an array in it, which is a different type. */
      bool wrap = csTypeIs(table, composite->inner, COMPOSITE_UNION) || csTypeIs(table, composite->inner, COMPOSITE_FUNCTION);
      if (wrap) append(buffer, "(");
      renderType(table, composite->inner, buffer, depth + 1);
      if (wrap) append(buffer, ")");
      append(buffer, "[]");
      return;
    }

    case COMPOSITE_FUNCTION:
      append(buffer, "(");
      for (int i = 0; i < composite->slotCount; i++) {
        if (i > 0) append(buffer, ", ");
        if (composite->hasRest && i == composite->slotCount - 1) append(buffer, "...");
        renderType(table, table->slots[composite->slotStart + i], buffer, depth + 1);
      }
      append(buffer, ") => ");
      renderType(table, composite->inner, buffer, depth + 1);
      return;

    case COMPOSITE_UNION:
      for (int i = 0; i < composite->slotCount; i++) {
        if (i > 0) append(buffer, " | ");
        renderType(table, table->slots[composite->slotStart + i], buffer, depth + 1);
      }
      return;
  }
  append(buffer, "%s", csTypeName(type));
}

const char *csTypeNameIn(const TypeTable *table, TypeId type) {
  if (!csTypeIsComposite(type)) return csTypeName(type);

  /* One message may name several types — "argument 1 is X but the parameter is
   * Y" — so the answers rotate rather than sharing one buffer. Nothing keeps a
   * name, and a message uses at most a few. */
  enum { BUFFERS = 6, WIDTH = 160 };
  static char buffers[BUFFERS][WIDTH];
  static int next = 0;

  NameBuffer buffer = {buffers[next], WIDTH, 0};
  next = (next + 1) % BUFFERS;
  buffer.out[0] = '\0';
  renderType(table, type, &buffer, 0);
  return buffer.out;
}

