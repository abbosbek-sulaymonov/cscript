/* object_internal.h — the seam between the object files.
 *
 * One thing, and it is the one thing they all do: every constructor, wherever
 * it lives, has to put the object it just allocated on the list the collector
 * walks. Nothing outside src/runtime/object*.c has any business calling it.
 */
#ifndef CSCRIPT_RUNTIME_OBJECT_INTERNAL_H
#define CSCRIPT_RUNTIME_OBJECT_INTERNAL_H

#include "cscript/object.h"

/* Links a freshly allocated object into the VM's allocation list and stamps
 * its type. Every constructor's second step, between allocating and filling
 * the fields in — which is the order that matters, because anything that
 * allocates while initialising has to find the object already rooted. */
void csObjectRegister(Obj *object, ObjType type);

#endif /* CSCRIPT_RUNTIME_OBJECT_INTERNAL_H */
