/* shape.h — hidden classes: the layout an object has, factored out of the
 * object itself.
 *
 * A property bag normally answers `o.x` with a hash and a probe. But the same
 * literal written in a loop produces a million objects with identical layouts,
 * and the name being looked up was known when the code was compiled. A shape
 * captures that layout once and shares it, so a property becomes an index into
 * a flat array — and an inline cache can remember the index.
 *
 * Shapes form a transition tree. The root is empty; adding a key produces a
 * child, and adding the same key to the same parent always finds that same
 * child. So `{}`, `{x}` and `{x, y}` are three nodes no matter how many
 * objects walk the path.
 *
 *     root ──x──▶ {x} ──y──▶ {x,y}
 *           └──a──▶ {a}
 *
 * The `transitions` edges are deliberately **weak**: a parent does not keep a
 * child alive. Otherwise the root — a permanent VM root — would pin every
 * layout any program ever built. The collector prunes edges to shapes nothing
 * else references.
 */
#ifndef CSCRIPT_SHAPE_H
#define CSCRIPT_SHAPE_H

#include "cscript/common.h"
#include "cscript/object.h"
#include "cscript/table.h"
#include "cscript/value.h"

/* Past this many properties an object stops transitioning and becomes an
 * ordinary hash table.
 *
 * The cost of a shape chain is quadratic: each node copies its parent's index
 * map and key list to gain one entry. That is a fine trade for the handful of
 * fields a record has, and a disaster for an object used as a dictionary and
 * filled key by key in a loop. Rather than let that case quietly degrade, it
 * gets a different representation. Inline caches simply miss on those objects.
 */
#define CS_SHAPE_MAX_SLOTS 64

struct Shape {
  Obj obj;

  Shape *parent; /* the shape this one adds a single key to */
  ObjString *key;       /* the key it adds; NULL at the root */
  int slotCount;

  /* name -> NUMBER_VAL(slot). Flat rather than a chain walk, so a cache miss
   * still costs one hash instead of one per property. */
  Table indices;

  /* Insertion order, which is also slot order. Shared by every object with
   * this shape, so Object.keys reads it instead of rebuilding it. */
  ObjString **keys;

  /* name -> OBJ_VAL(child shape). Weak; see the header comment. */
  Table transitions;
};

Shape *csShapeNewRoot(void);

/* The shape reached by adding `key` to `parent`, reusing an existing child
 * when there is one. Returns NULL when `parent` is already at the slot limit,
 * which is the caller's signal to fall back to dictionary mode.
 *
 * The result is held only by the weak transition edge that was just recorded.
 * A caller that allocates before storing it on an object must root it, or the
 * next collection will prune the edge and sweep the shape. */
Shape *csShapeTransition(Shape *parent, ObjString *key);

/* Writes the slot holding `key` and returns true, or returns false. */
bool csShapeLookup(Shape *shape, ObjString *key, int *slot);

/* GC hooks, called from object.c and memory.c alongside the other types. */
void csShapeBlacken(Shape *shape);
void csShapeFree(Shape *shape);

/* Drops transition edges whose child survived no other reference. Runs after
 * marking and before sweeping, like the string pool. */
void csShapePruneTransitions(Shape *shape);

#endif /* CSCRIPT_SHAPE_H */
