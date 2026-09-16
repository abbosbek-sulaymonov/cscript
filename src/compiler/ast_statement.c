/* ast_statement.c — the statement and declaration nodes.
 *
 * Split from the expression half only by size; they are the same three lines
 * each, over the same arena. The list-growing ones are here because the
 * statements are what have lists: a block, a program, a switch's cases.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/ast.h"

#include "compiler/ast_internal.h"

AstNode *csAstBlock(AstArena *arena, int line) {
  AstNode *node = csAstNewNode(arena, AST_BLOCK, line);
  if (node == NULL) return NULL;
  node->as.block.statements = NULL;
  node->as.block.count = 0;
  node->as.block.capacity = 0;
  return node;
}

AstNode *csAstIf(AstArena *arena, int line, AstNode *condition, AstNode *thenBranch, AstNode *elseBranch) {
  AstNode *node = csAstNewNode(arena, AST_IF_STMT, line);
  if (node == NULL) return NULL;
  node->as.ifStmt.condition = condition;
  node->as.ifStmt.thenBranch = thenBranch;
  node->as.ifStmt.elseBranch = elseBranch;
  return node;
}

AstNode *csAstWhile(AstArena *arena, int line, AstNode *condition, AstNode *body) {
  AstNode *node = csAstNewNode(arena, AST_WHILE_STMT, line);
  if (node == NULL) return NULL;
  node->as.whileStmt.condition = condition;
  node->as.whileStmt.body = body;
  node->as.whileStmt.isDoWhile = false;
  return node;
}

AstNode *csAstFor(AstArena *arena, int line, AstNode *initializer, AstNode *condition, AstNode *increment, AstNode *body) {
  AstNode *node = csAstNewNode(arena, AST_FOR_STMT, line);
  if (node == NULL) return NULL;
  node->as.forStmt.initializer = initializer;
  node->as.forStmt.condition = condition;
  node->as.forStmt.increment = increment;
  node->as.forStmt.body = body;
  return node;
}

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = csAstNewNode(arena, AST_EXPRESSION_STMT, line);
  if (node != NULL) node->as.expression = expression;
  return node;
}

AstNode *csAstFunction(AstArena *arena, int line, const char *name, int nameLength) {
  AstNode *node = csAstNewNode(arena, AST_FUNCTION, line);
  if (node == NULL) return NULL;
  node->as.function.name = name != NULL ? csAstInternName(arena, name, nameLength, &node->as.function.nameLength) : NULL;
  if (name == NULL) node->as.function.nameLength = 0;
  node->as.function.params = NULL;
  node->as.function.paramCount = 0;
  node->as.function.body = NULL;
  node->as.function.returnType = TYPE_DYNAMIC;
  node->as.function.hasReturnAnnotation = false;
  node->as.function.isAsync = false;
  node->as.function.nameIsInferred = false;
  node->as.function.isDeclaration = false;
  node->as.function.isMethod = false;
  node->as.function.isGenerator = false;
  node->as.function.isArrow = false;
  node->as.function.typeParamCount = 0;
  return node;
}

AstNode *csAstNew(AstArena *arena, int line, AstNode *callee) {
  AstNode *node = csAstCall(arena, line, callee);
  if (node != NULL) node->as.call.isNew = true;
  return node;
}

AstNode *csAstRegex(AstArena *arena, int line, const char *source, int sourceLength, const char *flags, int flagsLength) {
  AstNode *node = csAstNewNode(arena, AST_REGEX_LITERAL, line);
  if (node == NULL) return NULL;
  node->as.regex.source = csAstInternName(arena, source, sourceLength, &node->as.regex.sourceLength);
  node->as.regex.flags = csAstInternName(arena, flags, flagsLength, &node->as.regex.flagsLength);
  return node;
}

AstNode *csAstThis(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_THIS, line);
}

AstNode *csAstNewTarget(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_NEW_TARGET, line);
}

AstNode *csAstDynamicImport(AstArena *arena, int line, AstNode *specifier) {
  AstNode *node = csAstNewNode(arena, AST_DYNAMIC_IMPORT, line);
  if (node == NULL) return NULL;
  node->as.unary.op = UNARY_VOID; /* unused; the node has one operand */
  node->as.unary.operand = specifier;
  return node;
}

AstNode *csAstAwait(AstArena *arena, int line, AstNode *operand) {
  AstNode *node = csAstNewNode(arena, AST_AWAIT, line);
  if (node == NULL) return NULL;
  node->as.unary.operand = operand;
  return node;
}

AstNode *csAstSuper(AstArena *arena, int line, const char *name, int length) {
  AstNode *node = csAstNewNode(arena, AST_SUPER, line);
  if (node == NULL) return NULL;
  node->as.super.name = name != NULL ? csAstInternName(arena, name, length, &node->as.super.length) : NULL;
  if (name == NULL) node->as.super.length = 0;
  return node;
}

/* Both name lists grow by copying, like the others here: they are short and a
 * capacity field on every node would cost more than the copies do. */
static AstModuleName *addModuleName(AstArena *arena, AstModuleName *list, int count, const char *name, int nameLength, const char *alias, int aliasLength) {
  AstModuleName *grown = (AstModuleName *)csAstArenaAlloc(arena, sizeof(AstModuleName) * (size_t)(count + 1));
  if (grown == NULL) return NULL;
  if (list != NULL) memcpy(grown, list, sizeof(AstModuleName) * (size_t)count);

  grown[count].name = csAstInternName(arena, name, nameLength, &grown[count].nameLength);
  grown[count].alias = csAstInternName(arena, alias, aliasLength, &grown[count].aliasLength);
  return grown;
}

AstNode *csAstImport(AstArena *arena, int line, const char *specifier, int length) {
  AstNode *node = csAstNewNode(arena, AST_IMPORT, line);
  if (node == NULL) return NULL;
  node->as.import.specifier = csAstInternName(arena, specifier, length, &node->as.import.specifierLength);
  node->as.import.names = NULL;
  node->as.import.nameCount = 0;
  node->as.import.namespaceName = NULL;
  node->as.import.namespaceLength = 0;
  return node;
}

void csAstImportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength, const char *alias, int aliasLength) {
  if (node == NULL) return;
  AstModuleName *grown = addModuleName(arena, node->as.import.names, node->as.import.nameCount, name, nameLength, alias, aliasLength);
  if (grown == NULL) return;
  node->as.import.names = grown;
  node->as.import.nameCount++;
}

AstNode *csAstExport(AstArena *arena, int line, AstNode *declaration) {
  AstNode *node = csAstNewNode(arena, AST_EXPORT, line);
  if (node == NULL) return NULL;
  node->as.export.declaration = declaration;
  node->as.export.names = NULL;
  node->as.export.nameCount = 0;
  return node;
}

void csAstExportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength, const char *alias, int aliasLength) {
  if (node == NULL) return;
  AstModuleName *grown = addModuleName(arena, node->as.export.names, node->as.export.nameCount, name, nameLength, alias, aliasLength);
  if (grown == NULL) return;
  node->as.export.names = grown;
  node->as.export.nameCount++;
}

AstNode *csAstClass(AstArena *arena, int line, const char *name, int nameLength, const char *superName, int superLength) {
  AstNode *node = csAstNewNode(arena, AST_CLASS_DECL, line);
  if (node == NULL) return NULL;
  node->as.classDecl.name = csAstInternName(arena, name, nameLength, &node->as.classDecl.nameLength);
  node->as.classDecl.superName = superName != NULL ? csAstInternName(arena, superName, superLength, &node->as.classDecl.superLength) : NULL;
  if (superName == NULL) node->as.classDecl.superLength = 0;
  node->as.classDecl.fields = NULL;
  node->as.classDecl.fieldCount = 0;
  node->as.classDecl.members = NULL;
  node->as.classDecl.memberCount = 0;
  node->as.classDecl.constructor = NULL;
  return node;
}

void csAstClassAddField(AstArena *arena, AstNode *node, const char *name, int length, AstNode *initializer, TypeId declaredType, bool hasAnnotation, bool isStatic) {
  if (node == NULL) return;
  int count = node->as.classDecl.fieldCount;
  AstClassField *grown = (AstClassField *)csAstArenaAlloc(arena, sizeof(AstClassField) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (node->as.classDecl.fields != NULL) {
    memcpy(grown, node->as.classDecl.fields, sizeof(AstClassField) * (size_t)count);
  }
  grown[count].order = node->as.classDecl.fieldCount + node->as.classDecl.memberCount;
  grown[count].computedKey = NULL;
  int stored = 0;
  grown[count].name = csAstInternName(arena, name, length, &stored);
  grown[count].length = stored;
  grown[count].initializer = initializer;
  grown[count].declaredType = declaredType;
  grown[count].hasAnnotation = hasAnnotation;
  grown[count].isStatic = isStatic;
  node->as.classDecl.fields = grown;
  node->as.classDecl.fieldCount = count + 1;
}

void csAstClassAddMember(AstArena *arena, AstNode *node, AstNode *function, bool isStatic, ClassMemberKind kind) {
  if (node == NULL) return;
  int count = node->as.classDecl.memberCount;
  AstClassMember *grown = (AstClassMember *)csAstArenaAlloc(arena, sizeof(AstClassMember) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (node->as.classDecl.members != NULL) {
    memcpy(grown, node->as.classDecl.members, sizeof(AstClassMember) * (size_t)count);
  }
  grown[count].function = function;
  grown[count].isStatic = isStatic;
  grown[count].kind = kind;
  grown[count].order = node->as.classDecl.fieldCount + node->as.classDecl.memberCount;
  node->as.classDecl.members = grown;
  node->as.classDecl.memberCount = count + 1;
}

void csAstFunctionAddParam(AstArena *arena, AstNode *function, const char *name, int length, TypeId type, bool hasAnnotation) {
  if (function == NULL) return;

  /* Parameter lists are short, so growing by copy costs less than carrying a
   * capacity field on every function node. */
  int count = function->as.function.paramCount;
  AstParam *grown = (AstParam *)csAstArenaAlloc(arena, sizeof(AstParam) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (function->as.function.params != NULL) {
    memcpy(grown, function->as.function.params, sizeof(AstParam) * (size_t)count);
  }

  int stored = 0;
  grown[count].name = csAstInternName(arena, name, length, &stored);
  grown[count].length = stored;
  grown[count].type = type;
  grown[count].hasAnnotation = hasAnnotation;
  grown[count].pattern = NULL;
  grown[count].defaultValue = NULL;

  function->as.function.params = grown;
  function->as.function.paramCount = count + 1;
}

/* Grows an arena-backed pointer list by copying. The lists here are short, so
 * this costs less than carrying a capacity field on every node. */
AstNode **csAstGrowList(AstArena *arena, AstNode **list, int count) {
  AstNode **grown = (AstNode **)csAstArenaAlloc(arena, sizeof(AstNode *) * (size_t)(count + 1));
  if (grown == NULL) return NULL;
  if (list != NULL) memcpy(grown, list, sizeof(AstNode *) * (size_t)count);
  return grown;
}

AstNode *csAstConditional(AstArena *arena, int line, AstNode *condition, AstNode *thenValue, AstNode *elseValue) {
  AstNode *node = csAstNewNode(arena, AST_CONDITIONAL, line);
  if (node == NULL) return NULL;
  node->as.conditional.condition = condition;
  node->as.conditional.thenValue = thenValue;
  node->as.conditional.elseValue = elseValue;
  return node;
}

AstNode *csAstBreak(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_BREAK_STMT, line);
}

AstNode *csAstLabeled(AstArena *arena, int line, const char *name, int length, AstNode *body) {
  AstNode *node = csAstNewNode(arena, AST_LABELED_STMT, line);
  if (node == NULL) return NULL;
  node->as.labeled.name = name;
  node->as.labeled.length = length;
  node->as.labeled.body = body;
  return node;
}

AstNode *csAstContinue(AstArena *arena, int line) {
  return csAstNewNode(arena, AST_CONTINUE_STMT, line);
}

AstNode *csAstSwitch(AstArena *arena, int line, AstNode *subject) {
  AstNode *node = csAstNewNode(arena, AST_SWITCH_STMT, line);
  if (node == NULL) return NULL;
  node->as.switchStmt.subject = subject;
  node->as.switchStmt.cases = NULL;
  node->as.switchStmt.caseCount = 0;
  node->as.switchStmt.defaultBody = NULL;
  return node;
}

void csAstSwitchAddCase(AstArena *arena, AstNode *node, AstNode *test, AstNode *body) {
  if (node == NULL) return;
  int count = node->as.switchStmt.caseCount;
  AstSwitchCase *grown = (AstSwitchCase *)csAstArenaAlloc(arena, sizeof(AstSwitchCase) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (node->as.switchStmt.cases != NULL) {
    memcpy(grown, node->as.switchStmt.cases, sizeof(AstSwitchCase) * (size_t)count);
  }
  grown[count].test = test;
  grown[count].body = body;
  node->as.switchStmt.cases = grown;
  node->as.switchStmt.caseCount = count + 1;
}

AstNode *csAstIndex(AstArena *arena, int line, AstNode *target, AstNode *index) {
  AstNode *node = csAstNewNode(arena, AST_INDEX, line);
  if (node == NULL) return NULL;
  node->as.index.target = target;
  node->as.index.index = index;
  return node;
}

AstNode *csAstObjectLiteral(AstArena *arena, int line) {
  AstNode *node = csAstNewNode(arena, AST_OBJECT_LITERAL, line);
  if (node == NULL) return NULL;
  node->as.objectLiteral.keys = NULL;
  node->as.objectLiteral.values = NULL;
  node->as.objectLiteral.count = 0;
  return node;
}

void csAstObjectLiteralAddKind(AstArena *arena, AstNode *object, AstNode *key, AstNode *value, ObjectEntryKind kind) {
  csAstObjectLiteralAdd(arena, object, key, value);
  if (object == NULL || object->as.objectLiteral.count == 0) return;
  object->as.objectLiteral.kinds[object->as.objectLiteral.count - 1] = (uint8_t)kind;
}

void csAstObjectLiteralAdd(AstArena *arena, AstNode *object, AstNode *key, AstNode *value) {
  /* A NULL key marks `...value`, which has no key by construction. */
  if (object == NULL || value == NULL) return;
  int count = object->as.objectLiteral.count;

  AstNode **keys = csAstGrowList(arena, object->as.objectLiteral.keys, count);
  AstNode **values = csAstGrowList(arena, object->as.objectLiteral.values, count);
  if (keys == NULL || values == NULL) return;

  uint8_t *kinds = (uint8_t *)csAstArenaAlloc(arena, (size_t)(count + 1));
  if (kinds == NULL) return;
  if (count > 0) memcpy(kinds, object->as.objectLiteral.kinds, (size_t)count);

  keys[count] = key;
  values[count] = value;
  kinds[count] = key == NULL ? (uint8_t)OBJECT_ENTRY_SPREAD : (uint8_t)OBJECT_ENTRY_VALUE;
  object->as.objectLiteral.keys = keys;
  object->as.objectLiteral.values = values;
  object->as.objectLiteral.kinds = kinds;
  object->as.objectLiteral.count = count + 1;
}

AstNode *csAstArrayLiteral(AstArena *arena, int line) {
  AstNode *node = csAstNewNode(arena, AST_ARRAY_LITERAL, line);
  if (node == NULL) return NULL;
  node->as.arrayLiteral.elements = NULL;
  node->as.arrayLiteral.count = 0;
  return node;
}

void csAstArrayLiteralAdd(AstArena *arena, AstNode *array, AstNode *element) {
  if (array == NULL || element == NULL) return;
  int count = array->as.arrayLiteral.count;
  AstNode **grown = csAstGrowList(arena, array->as.arrayLiteral.elements, count);
  if (grown == NULL) return;
  grown[count] = element;
  array->as.arrayLiteral.elements = grown;
  array->as.arrayLiteral.count = count + 1;
}

AstNode *csAstForOf(AstArena *arena, int line, const char *name, int nameLength, bool isConst, AstNode *iterable, AstNode *body) {
  AstNode *node = csAstNewNode(arena, AST_FOR_OF_STMT, line);
  if (node == NULL) return NULL;
  node->as.forOf.name = csAstInternName(arena, name, nameLength, &node->as.forOf.nameLength);
  node->as.forOf.isConst = isConst;
  node->as.forOf.iterable = iterable;
  node->as.forOf.body = body;
  node->as.forOf.isForIn = false;
  node->as.forOf.pattern = NULL;
  return node;
}

AstNode *csAstTry(AstArena *arena, int line, AstNode *body, const char *catchName, int catchNameLength, AstNode *catchBody, AstNode *finallyBody) {
  AstNode *node = csAstNewNode(arena, AST_TRY_STMT, line);
  if (node == NULL) return NULL;
  node->as.tryStmt.body = body;
  node->as.tryStmt.catchName = catchName != NULL ? csAstInternName(arena, catchName, catchNameLength, &node->as.tryStmt.catchNameLength) : NULL;
  if (catchName == NULL) node->as.tryStmt.catchNameLength = 0;
  node->as.tryStmt.catchBody = catchBody;
  node->as.tryStmt.finallyBody = finallyBody;
  return node;
}

AstNode *csAstSpread(AstArena *arena, int line, AstNode *expression) {
  AstNode *node = csAstNewNode(arena, AST_SPREAD, line);
  if (node != NULL) node->as.spread = expression;
  return node;
}

void csAstDestructureNest(AstNode *node, AstNode *pattern) {
  if (node == NULL || node->as.destructure.count == 0) return;
  node->as.destructure.bindings[node->as.destructure.count - 1].pattern = pattern;
}

void csAstParamPattern(AstNode *function, AstNode *pattern) {
  if (function == NULL || function->as.function.paramCount == 0) return;
  function->as.function.params[function->as.function.paramCount - 1].pattern = pattern;
}

AstNode *csAstDestructure(AstArena *arena, int line, bool isObject, bool isConst) {
  AstNode *node = csAstNewNode(arena, AST_DESTRUCTURE, line);
  if (node == NULL) return NULL;
  node->as.destructure.bindings = NULL;
  node->as.destructure.count = 0;
  node->as.destructure.isObject = isObject;
  node->as.destructure.isConst = isConst;
  node->as.destructure.initializer = NULL;
  return node;
}

void csAstDestructureAdd(AstArena *arena, AstNode *node, const char *key, int keyLength, const char *name, int nameLength, AstNode *defaultValue, bool isRest) {
  if (node == NULL) return;
  int count = node->as.destructure.count;

  AstBinding *grown = (AstBinding *)csAstArenaAlloc(arena, sizeof(AstBinding) * (size_t)(count + 1));
  if (grown == NULL) return;
  if (node->as.destructure.bindings != NULL) {
    memcpy(grown, node->as.destructure.bindings, sizeof(AstBinding) * (size_t)count);
  }

  int stored = 0;
  grown[count].key = key != NULL ? csAstInternName(arena, key, keyLength, &stored) : NULL;
  grown[count].keyLength = key != NULL ? stored : 0;
  grown[count].name = csAstInternName(arena, name, nameLength, &stored);
  grown[count].nameLength = stored;
  grown[count].defaultValue = defaultValue;
  grown[count].isRest = isRest;
  grown[count].pattern = NULL;

  node->as.destructure.bindings = grown;
  node->as.destructure.count = count + 1;
}

AstNode *csAstThrow(AstArena *arena, int line, AstNode *thrown) {
  AstNode *node = csAstNewNode(arena, AST_THROW_STMT, line);
  if (node != NULL) node->as.thrown = thrown;
  return node;
}

AstNode *csAstReturn(AstArena *arena, int line, AstNode *value) {
  AstNode *node = csAstNewNode(arena, AST_RETURN_STMT, line);
  if (node != NULL) node->as.returnValue = value;
  return node;
}

void csAstProgramAddExport(AstNode *program, const char *name, int length, TypeId type) {
  if (program == NULL || program->type != AST_PROGRAM) return;
  AstArena *arena = program->as.program.arena;
  if (arena == NULL) return;
  int count = program->as.program.exportTypeCount;
  AstExportType *grown = (AstExportType *)csAstArenaAlloc(arena, sizeof(AstExportType) * (size_t)(count + 1));
  if (grown == NULL) return;
  for (int i = 0; i < count; i++) grown[i] = program->as.program.exportTypes[i];
  grown[count].name = csAstInternName(arena, name, length, &grown[count].length);
  grown[count].type = type;
  program->as.program.exportTypes = grown;
  program->as.program.exportTypeCount = count + 1;
}

TypeTable *csAstProgramTakeTypes(AstNode *program) {
  if (program == NULL || program->type != AST_PROGRAM) return NULL;
  TypeTable *types = program->as.program.types;
  program->as.program.types = NULL;
  return types;
}

void csAstProgramFreeTypes(AstNode *program) {
  if (program == NULL || program->type != AST_PROGRAM) return;
  free(program->as.program.types);
  program->as.program.types = NULL;
}

AstNode *csAstProgram(AstArena *arena, int line) {
  AstNode *node = csAstNewNode(arena, AST_PROGRAM, line);
  if (node == NULL) return NULL;
  node->as.program.statements = NULL;
  node->as.program.count = 0;
  node->as.program.capacity = 0;
  /* Not in the arena: the table outlives the tree, because an importer reads
   * what a module exports long after the parse that produced it. */
  node->as.program.types = (TypeTable *)malloc(sizeof(TypeTable));
  if (node->as.program.types != NULL) csTypeTableInit(node->as.program.types);
  node->as.program.exportTypes = NULL;
  node->as.program.exportTypeCount = 0;
  node->as.program.arena = arena;
  return node;
}

void csAstProgramAdd(AstArena *arena, AstNode *parent, AstNode *statement) {
  if (parent == NULL || statement == NULL) return;

  /* AST_PROGRAM and AST_BLOCK have the same list layout, so one appender
   * serves both. */
  AstNode *program = parent;

  if (program->as.program.count + 1 > program->as.program.capacity) {
    int oldCapacity = program->as.program.capacity;
    int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;

    /* The arena cannot resize in place, so grow by copying. Statement lists are
     * short and this doubles, so the wasted space stays bounded. */
    AstNode **grown = (AstNode **)csAstArenaAlloc(arena, sizeof(AstNode *) * (size_t)newCapacity);
    if (grown == NULL) return;
    if (program->as.program.statements != NULL) {
      memcpy(grown, program->as.program.statements, sizeof(AstNode *) * (size_t)oldCapacity);
    }
    program->as.program.statements = grown;
    program->as.program.capacity = newCapacity;
  }

  program->as.program.statements[program->as.program.count++] = statement;
}
