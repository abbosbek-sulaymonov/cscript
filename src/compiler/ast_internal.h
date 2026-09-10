/* ast_internal.h — the two things the AST files share.
 *
 * Nodes are never freed individually, so there is no destructor and no
 * ownership to describe: the arena is reset or destroyed as a unit once
 * compilation is done. What crosses between the files is only how a node and a
 * list are made. Nothing outside src/compiler/ast*.c includes this.
 */
#ifndef CSCRIPT_COMPILER_AST_INTERNAL_H
#define CSCRIPT_COMPILER_AST_INTERNAL_H

#include "cscript/ast.h"

/* A zeroed node of the given type, from the arena. */
AstNode *csAstNewNode(AstArena *arena, AstNodeType type, int line);

/* Grows a node list by one, copying into a larger arena allocation.
 *
 * The old block is not freed — nothing in an arena is — so this trades memory
 * for never having to think about a dangling pointer into a list that moved. */
AstNode **csAstGrowList(AstArena *arena, AstNode **list, int count);

/* Copies a name into the arena and NUL-terminates it.
 *
 * A node otherwise points into the source text, which is fine while the source
 * outlives the tree — and it does. A *name* is copied because the compiler
 * hands it to csStringCopy, which wants a run it can measure. */
const char *csAstInternName(AstArena *arena, const char *name, int length,
                            int *lengthOut);

#endif /* CSCRIPT_COMPILER_AST_INTERNAL_H */
