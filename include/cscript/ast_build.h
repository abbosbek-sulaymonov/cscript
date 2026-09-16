/* ast_build.h — the arena, and one constructor per node type.
 *
 * Nodes are never freed individually: the arena is reset or destroyed as a
 * unit once compilation is done, which is why there is no destructor here and
 * no ownership to describe. Included from the end of ast.h rather than
 * separately, because nothing can call these without the node shapes they
 * build — so there is exactly one header to include and it is that one.
 */
#ifndef CSCRIPT_AST_BUILD_H
#define CSCRIPT_AST_BUILD_H

#include "cscript/ast.h"

/* Bump allocator. Nodes are never freed individually; the arena is reset or
 * destroyed as a unit once compilation is done. */
typedef struct AstArenaBlock AstArenaBlock;

struct AstArena {
  AstArenaBlock *head;
  size_t bytesAllocated;
};

void csAstArenaInit(AstArena *arena);
void csAstArenaFree(AstArena *arena);
void *csAstArenaAlloc(AstArena *arena, size_t size);

/* Node constructors. Every one allocates from `arena`. */
AstNode *csAstNumber(AstArena *arena, int line, double value);
AstNode *csAstString(AstArena *arena, int line, const char *chars, int length);
/* `chars` is the literal as written, without the trailing `n`. */
AstNode *csAstBigInt(AstArena *arena, int line, const char *chars, int length);
AstNode *csAstBool(AstArena *arena, int line, bool value);
AstNode *csAstNull(AstArena *arena, int line);
AstNode *csAstUndefined(AstArena *arena, int line);
AstNode *csAstUnary(AstArena *arena, int line, UnaryOp op, AstNode *operand);
AstNode *csAstBinary(AstArena *arena, int line, BinaryOp op, AstNode *left, AstNode *right);
AstNode *csAstLogical(AstArena *arena, int line, LogicalOp op, AstNode *left, AstNode *right);
AstNode *csAstGrouping(AstArena *arena, int line, AstNode *inner);
AstNode *csAstIdentifier(AstArena *arena, int line, const char *name, int length);
AstNode *csAstAssign(AstArena *arena, int line, AstNode *target, AstNode *value);

/* `target op= value`, with the kind kept rather than expanded. */
AstNode *csAstAssignKind(AstArena *arena, int line, AstNode *target, AstNode *value, AssignKind kind, BinaryOp compoundOp);

/* Wraps the whole of a postfix chain containing at least one `?.`.
 *
 * Optional chaining short-circuits the chain, not the link: in `a?.b.c()` a
 * nullish `a` skips the `.c` and the call too. The links therefore cannot
 * decide on their own where to jump, so the outermost expression carries the
 * landing site and each link jumps to it. */
AstNode *csAstOptionalChain(AstArena *arena, int line, AstNode *expression);

/* `delete o.k`. The target is a property or an index; anything else is
 * reported where it is written. */
AstNode *csAstDelete(AstArena *arena, int line, AstNode *target);

/* `yield v` and `yield* xs`. An expression: it produces whatever the next
 * `next(x)` passes back in. */
AstNode *csAstYield(AstArena *arena, int line, AstNode *value, bool isDelegate);

/* `a, b` — the comma operator, not a separator. */
AstNode *csAstSequence(AstArena *arena, int line, AstNode *first, AstNode *second);

/* The first argument a tagged template hands its tag: the literal pieces, with
 * the unescaped text hung off them as `raw`. */
AstNode *csAstTemplateStrings(AstArena *arena, int line, AstNode *cooked, AstNode *raw);
AstNode *csAstUpdate(AstArena *arena, int line, AstNode *target, bool isIncrement, bool isPrefix);
AstNode *csAstCall(AstArena *arena, int line, AstNode *callee);
AstNode *csAstNew(AstArena *arena, int line, AstNode *callee);
AstNode *csAstThis(AstArena *arena, int line);
AstNode *csAstNewTarget(AstArena *arena, int line);
AstNode *csAstDynamicImport(AstArena *arena, int line, AstNode *specifier);
AstNode *csAstRegex(AstArena *arena, int line, const char *source, int sourceLength, const char *flags, int flagsLength);
AstNode *csAstAwait(AstArena *arena, int line, AstNode *operand);
AstNode *csAstSuper(AstArena *arena, int line, const char *name, int length);
AstNode *csAstImport(AstArena *arena, int line, const char *specifier, int length);
void csAstImportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength, const char *alias, int aliasLength);
AstNode *csAstExport(AstArena *arena, int line, AstNode *declaration);
void csAstExportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength, const char *alias, int aliasLength);

AstNode *csAstClass(AstArena *arena, int line, const char *name, int nameLength, const char *superName, int superLength);
void csAstClassAddField(AstArena *arena, AstNode *node, const char *name, int length, AstNode *initializer, TypeId declaredType, bool hasAnnotation, bool isStatic);
void csAstClassAddMember(AstArena *arena, AstNode *node, AstNode *function, bool isStatic, ClassMemberKind kind);
void csAstCallAddArgument(AstArena *arena, AstNode *call, AstNode *argument);
AstNode *csAstProperty(AstArena *arena, int line, AstNode *object, const char *name, int length);

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression);
AstNode *csAstVarDecl(AstArena *arena, int line, const char *name, int length, AstNode *initializer, bool isConst, TypeId declaredType, bool hasAnnotation);
AstNode *csAstBlock(AstArena *arena, int line);
AstNode *csAstIf(AstArena *arena, int line, AstNode *condition, AstNode *thenBranch, AstNode *elseBranch);
AstNode *csAstWhile(AstArena *arena, int line, AstNode *condition, AstNode *body);
AstNode *csAstFor(AstArena *arena, int line, AstNode *initializer, AstNode *condition, AstNode *increment, AstNode *body);
AstNode *csAstFunction(AstArena *arena, int line, const char *name, int nameLength);
void csAstFunctionAddParam(AstArena *arena, AstNode *function, const char *name, int length, TypeId type, bool hasAnnotation);
AstNode *csAstConditional(AstArena *arena, int line, AstNode *condition, AstNode *thenValue, AstNode *elseValue);
AstNode *csAstBreak(AstArena *arena, int line);

/* `name: statement`. The label is a jump target, not a binding. */
AstNode *csAstLabeled(AstArena *arena, int line, const char *name, int length, AstNode *body);
AstNode *csAstContinue(AstArena *arena, int line);
AstNode *csAstSwitch(AstArena *arena, int line, AstNode *subject);
void csAstSwitchAddCase(AstArena *arena, AstNode *node, AstNode *test, AstNode *body);
AstNode *csAstIndex(AstArena *arena, int line, AstNode *target, AstNode *index);
AstNode *csAstObjectLiteral(AstArena *arena, int line);
/* `key` is NULL for a `...value` entry, which has no key. */
void csAstObjectLiteralAdd(AstArena *arena, AstNode *object, AstNode *key, AstNode *value);
/* Same, for an entry that is not a plain `key: value`. */
void csAstObjectLiteralAddKind(AstArena *arena, AstNode *object, AstNode *key, AstNode *value, ObjectEntryKind kind);
AstNode *csAstArrayLiteral(AstArena *arena, int line);
void csAstArrayLiteralAdd(AstArena *arena, AstNode *array, AstNode *element);
AstNode *csAstForOf(AstArena *arena, int line, const char *name, int nameLength, bool isConst, AstNode *iterable, AstNode *body);
AstNode *csAstTry(AstArena *arena, int line, AstNode *body, const char *catchName, int catchNameLength, AstNode *catchBody, AstNode *finallyBody);
AstNode *csAstThrow(AstArena *arena, int line, AstNode *thrown);
AstNode *csAstSpread(AstArena *arena, int line, AstNode *expression);
AstNode *csAstDestructure(AstArena *arena, int line, bool isObject, bool isConst);
void csAstDestructureAdd(AstArena *arena, AstNode *node, const char *key, int keyLength, const char *name, int nameLength, AstNode *defaultValue, bool isRest);

/* Attaches a nested pattern to the binding just added. */
void csAstDestructureNest(AstNode *node, AstNode *pattern);

/* Attaches a pattern to the parameter just added. */
void csAstParamPattern(AstNode *function, AstNode *pattern);
AstNode *csAstReturn(AstArena *arena, int line, AstNode *value);
AstNode *csAstProgram(AstArena *arena, int line);

/* Records an exported binding's type. Called by the checker, once per name. */
void csAstProgramAddExport(AstNode *program, const char *name, int length, TypeId type);

/* Hands the program's type table to the caller, who then owns it. */
TypeTable *csAstProgramTakeTypes(AstNode *program);

/* Frees it, when nobody took it. */
void csAstProgramFreeTypes(AstNode *program);

/* Appends to a AST_PROGRAM or AST_BLOCK statement list. */
void csAstProgramAdd(AstArena *arena, AstNode *parent, AstNode *statement);

const char *csUnaryOpName(UnaryOp op);
const char *csBinaryOpName(BinaryOp op);
const char *csLogicalOpName(LogicalOp op);

#endif /* CSCRIPT_AST_BUILD_H */
