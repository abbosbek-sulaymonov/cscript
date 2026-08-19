/* ast.h — the parse tree, and the arena it lives in.
 *
 * The AST is a compile-time-only structure: the compiler lowers it to bytecode
 * and the whole arena is then released in one call. Because of that, no AST
 * node ever holds a GC-managed object — string and identifier literals keep a
 * slice of the source and are interned later, by the compiler.
 */
#ifndef CSCRIPT_AST_H
#define CSCRIPT_AST_H

#include "cscript/common.h"
#include "cscript/type.h"

typedef enum {
  /* Expressions. */
  AST_NUMBER_LITERAL,
  AST_STRING_LITERAL,
  AST_BOOL_LITERAL,
  AST_NULL_LITERAL,
  AST_UNDEFINED_LITERAL,
  AST_UNARY,
  AST_BINARY,
  AST_LOGICAL,
  AST_GROUPING,
  AST_IDENTIFIER,
  AST_ASSIGN,
  AST_UPDATE,
  AST_CALL,
  AST_PROPERTY,
  AST_FUNCTION,
  AST_INDEX,
  AST_OBJECT_LITERAL,
  AST_ARRAY_LITERAL,
  AST_SPREAD,
  AST_CONDITIONAL,
  AST_THIS,
  AST_SUPER,
  AST_AWAIT,

  /* Statements. */
  AST_EXPRESSION_STMT,
  AST_VAR_DECL,
  AST_DESTRUCTURE,
  AST_BLOCK,
  AST_IF_STMT,
  AST_WHILE_STMT,
  AST_FOR_STMT,
  AST_FOR_OF_STMT,
  AST_RETURN_STMT,
  AST_BREAK_STMT,
  AST_CONTINUE_STMT,
  AST_SWITCH_STMT,
  AST_TRY_STMT,
  AST_THROW_STMT,
  AST_CLASS_DECL,
  AST_IMPORT,
  AST_EXPORT,
  AST_PROGRAM,
} AstNodeType;

typedef enum {
  UNARY_NEGATE, /* -x */
  UNARY_NOT,    /* !x */
  UNARY_TYPEOF, /* typeof x */
} UnaryOp;

typedef enum {
  BINARY_ADD, BINARY_SUBTRACT, BINARY_MULTIPLY, BINARY_DIVIDE, BINARY_MODULO,
  BINARY_EXPONENT,
  BINARY_EQUAL, BINARY_NOT_EQUAL, /* === !== — CScript has no coercing equality */
  BINARY_GREATER, BINARY_GREATER_EQUAL,
  BINARY_LESS, BINARY_LESS_EQUAL,
  BINARY_INSTANCEOF,
} BinaryOp;

typedef enum {
  LOGICAL_AND, /* && */
  LOGICAL_OR,  /* || */
} LogicalOp;

typedef struct AstNode AstNode;

/* One `case` arm. The body is a statement list rather than a block, because
 * cases share a scope in JavaScript. */
typedef struct AstSwitchCase {
  AstNode *test;
  AstNode *body; /* an AST_BLOCK holding the arm's statements */
} AstSwitchCase;

/* One name bound by a destructuring pattern.
 *
 * `key` is what to read from the source — an array index is implied by
 * position, an object property by name. `name` is what the binding is called,
 * which differs when the pattern renames: `{ x: a }`. */
typedef struct AstBinding {
  const char *key;
  int keyLength;
  const char *name;
  int nameLength;
  AstNode *defaultValue; /* NULL when the pattern has no default */
  bool isRest;           /* ...rest, only valid last in an array pattern */
} AstBinding;

/* One name moved across a module boundary: `a`, or `a as b`. The alias is the
 * local name, and equals the exported name when there is no `as`. */
typedef struct AstModuleName {
  const char *name;
  int nameLength;
  const char *alias;
  int aliasLength;
} AstModuleName;

/* One field declared in a class body: `x;`, `x = 0;` or `x: number = 0;`.
 *
 * Field initialisers are lowered into the class's hidden field-initialiser
 * method rather than into the constructor, so a class without a constructor
 * still gets them and a subclass does not have to remember to run them. */
typedef struct AstClassField {
  const char *name;
  int length;
  AstNode *initializer; /* NULL for a bare `x;` */
  TypeKind declaredType;
  bool hasAnnotation;
} AstClassField;

/* One method in a class body. `constructor` is pulled out separately. */
typedef struct AstClassMember {
  AstNode *function; /* an AST_FUNCTION */
  bool isStatic;
} AstClassMember;

/* One declared parameter. Stored inline in the function node's array. */
typedef struct AstParam {
  const char *name;
  int length;
  TypeKind type;
  bool hasAnnotation;
} AstParam;

struct AstNode {
  AstNodeType type;
  int line;

  /* Filled in by the type checker. The compiler reads it to specialise code,
   * which is why annotations are consumed rather than erased. TYPE_ANY means
   * "not known statically", not "unchecked". */
  TypeKind resolvedType;
  union {
    double number;                       /* AST_NUMBER_LITERAL */
    bool boolean;                        /* AST_BOOL_LITERAL */
    struct {                             /* AST_STRING_LITERAL */
      const char *chars;                 /*   decoded, arena-owned */
      int length;
    } string;
    struct {                             /* AST_UNARY */
      UnaryOp op;
      AstNode *operand;
    } unary;
    struct {                             /* AST_BINARY */
      BinaryOp op;
      AstNode *left;
      AstNode *right;
    } binary;
    struct {                             /* AST_LOGICAL */
      LogicalOp op;
      AstNode *left;
      AstNode *right;
    } logical;
    AstNode *grouping;                   /* AST_GROUPING */
    AstNode *expression;                 /* AST_EXPRESSION_STMT */
    struct {                             /* AST_IDENTIFIER */
      const char *name;                  /*   arena-owned, NUL-terminated */
      int length;
    } identifier;
    struct {                             /* AST_ASSIGN */
      AstNode *target;                   /*   an identifier for now */
      AstNode *value;
    } assign;
    struct {                             /* AST_UPDATE — ++x, x++, --x, x-- */
      AstNode *target;
      bool isIncrement;
      bool isPrefix;                     /*   prefix yields the new value */
    } update;
    struct {                             /* AST_CALL, and `new C(...)` */
      AstNode *callee;
      AstNode **arguments;
      int argCount;
      bool isNew;                        /*   construction rather than a call */
    } call;
    struct {                             /* AST_SUPER */
      const char *name;                  /*   NULL for `super(...)` */
      int length;
    } super;
    struct {                             /* AST_IMPORT */
      const char *specifier;             /*   the quoted path, decoded */
      int specifierLength;
      struct AstModuleName *names;
      int nameCount;
      const char *namespaceName;         /*   non-NULL for `import * as ns` */
      int namespaceLength;
    } import;
    struct {                             /* AST_EXPORT */
      AstNode *declaration;              /*   NULL for `export { a, b };` */
      struct AstModuleName *names;       /*   only for the list form */
      int nameCount;
    } export;
    struct {                             /* AST_CLASS_DECL */
      const char *name;
      int nameLength;
      const char *superName;             /*   NULL without `extends` */
      int superLength;
      struct AstClassField *fields;
      int fieldCount;
      struct AstClassMember *members;
      int memberCount;
      AstNode *constructor;              /*   an AST_FUNCTION, or NULL */
    } classDecl;
    struct {                             /* AST_PROPERTY — obj.name */
      AstNode *object;
      const char *name;
      int length;
    } property;
    struct {                             /* AST_VAR_DECL */
      const char *name;
      int length;
      AstNode *initializer;              /*   NULL for `let x;` */
      bool isConst;
      TypeKind declaredType;             /*   from `: T`, else TYPE_ANY */
      bool hasAnnotation;                /*   distinguishes `: any` from none */
    } varDecl;
    struct {                             /* AST_BLOCK */
      AstNode **statements;
      int count;
      int capacity;
    } block;
    struct {                             /* AST_IF_STMT */
      AstNode *condition;
      AstNode *thenBranch;
      AstNode *elseBranch;               /*   NULL when there is no else */
    } ifStmt;
    struct {                             /* AST_WHILE_STMT */
      AstNode *condition;
      AstNode *body;
    } whileStmt;
    struct {                             /* AST_FOR_STMT — any clause may be NULL */
      AstNode *initializer;
      AstNode *condition;
      AstNode *increment;
      AstNode *body;
    } forStmt;
    AstNode *spread;                     /* AST_SPREAD — ...expr */
    struct {                             /* AST_DESTRUCTURE */
      struct AstBinding *bindings;
      int count;
      bool isObject;                     /*   {a, b} rather than [a, b] */
      bool isConst;
      AstNode *initializer;
    } destructure;
    struct {                             /* AST_CONDITIONAL — c ? a : b */
      AstNode *condition;
      AstNode *thenValue;
      AstNode *elseValue;
    } conditional;
    struct {                             /* AST_SWITCH_STMT */
      AstNode *subject;
      struct AstSwitchCase *cases;
      int caseCount;
      AstNode *defaultBody;              /*   an AST_BLOCK, or NULL */
    } switchStmt;
    struct {                             /* AST_INDEX — target[index] */
      AstNode *target;
      AstNode *index;
    } index;
    struct {                             /* AST_OBJECT_LITERAL */
      AstNode **keys;                    /*   string literal nodes */
      AstNode **values;
      int count;
    } objectLiteral;
    struct {                             /* AST_ARRAY_LITERAL */
      AstNode **elements;
      int count;
    } arrayLiteral;
    struct {                             /* AST_FUNCTION */
      const char *name;                  /*   NULL for a function expression */
      int nameLength;
      struct AstParam *params;
      int paramCount;
      AstNode *body;                     /*   an AST_BLOCK */
      TypeKind returnType;
      bool hasReturnAnnotation;
      bool isAsync;                      /*   returns a promise, may await */
      /* True when the name came from the binding the function was assigned to
       * rather than from a `function name(...)` declaration. Such a name is
       * for diagnostics only: it must not declare anything, or `const f = () =>
       * 1;` would bind `f` twice. */
      bool nameIsInferred;
    } function;
    AstNode *returnValue;                /* AST_RETURN_STMT, may be NULL */
    struct {                             /* AST_FOR_OF_STMT */
      const char *name;                  /*   the loop binding */
      int nameLength;
      bool isConst;
      AstNode *iterable;
      AstNode *body;
    } forOf;
    struct {                             /* AST_TRY_STMT */
      AstNode *body;                     /*   an AST_BLOCK */
      const char *catchName;             /*   NULL for `catch { }` */
      int catchNameLength;
      AstNode *catchBody;                /*   NULL when there is no catch */
      AstNode *finallyBody;              /*   NULL when there is no finally */
    } tryStmt;
    AstNode *thrown;                     /* AST_THROW_STMT */
    struct {                             /* AST_PROGRAM */
      AstNode **statements;
      int count;
      int capacity;
    } program;
  } as;
};

/* Bump allocator. Nodes are never freed individually; the arena is reset or
 * destroyed as a unit once compilation is done. */
typedef struct AstArenaBlock AstArenaBlock;

typedef struct {
  AstArenaBlock *head;
  size_t bytesAllocated;
} AstArena;

void csAstArenaInit(AstArena *arena);
void csAstArenaFree(AstArena *arena);
void *csAstArenaAlloc(AstArena *arena, size_t size);

/* Node constructors. Every one allocates from `arena`. */
AstNode *csAstNumber(AstArena *arena, int line, double value);
AstNode *csAstString(AstArena *arena, int line, const char *chars, int length);
AstNode *csAstBool(AstArena *arena, int line, bool value);
AstNode *csAstNull(AstArena *arena, int line);
AstNode *csAstUndefined(AstArena *arena, int line);
AstNode *csAstUnary(AstArena *arena, int line, UnaryOp op, AstNode *operand);
AstNode *csAstBinary(AstArena *arena, int line, BinaryOp op, AstNode *left,
                     AstNode *right);
AstNode *csAstLogical(AstArena *arena, int line, LogicalOp op, AstNode *left,
                      AstNode *right);
AstNode *csAstGrouping(AstArena *arena, int line, AstNode *inner);
AstNode *csAstIdentifier(AstArena *arena, int line, const char *name, int length);
AstNode *csAstAssign(AstArena *arena, int line, AstNode *target, AstNode *value);
AstNode *csAstUpdate(AstArena *arena, int line, AstNode *target, bool isIncrement,
                     bool isPrefix);
AstNode *csAstCall(AstArena *arena, int line, AstNode *callee);
AstNode *csAstNew(AstArena *arena, int line, AstNode *callee);
AstNode *csAstThis(AstArena *arena, int line);
AstNode *csAstAwait(AstArena *arena, int line, AstNode *operand);
AstNode *csAstSuper(AstArena *arena, int line, const char *name, int length);
AstNode *csAstImport(AstArena *arena, int line, const char *specifier, int length);
void csAstImportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength,
                        const char *alias, int aliasLength);
AstNode *csAstExport(AstArena *arena, int line, AstNode *declaration);
void csAstExportAddName(AstArena *arena, AstNode *node, const char *name, int nameLength,
                        const char *alias, int aliasLength);

AstNode *csAstClass(AstArena *arena, int line, const char *name, int nameLength,
                    const char *superName, int superLength);
void csAstClassAddField(AstArena *arena, AstNode *node, const char *name, int length,
                        AstNode *initializer, TypeKind declaredType,
                        bool hasAnnotation);
void csAstClassAddMember(AstArena *arena, AstNode *node, AstNode *function,
                         bool isStatic);
void csAstCallAddArgument(AstArena *arena, AstNode *call, AstNode *argument);
AstNode *csAstProperty(AstArena *arena, int line, AstNode *object, const char *name,
                       int length);

AstNode *csAstExpressionStmt(AstArena *arena, int line, AstNode *expression);
AstNode *csAstVarDecl(AstArena *arena, int line, const char *name, int length,
                      AstNode *initializer, bool isConst, TypeKind declaredType,
                      bool hasAnnotation);
AstNode *csAstBlock(AstArena *arena, int line);
AstNode *csAstIf(AstArena *arena, int line, AstNode *condition, AstNode *thenBranch,
                 AstNode *elseBranch);
AstNode *csAstWhile(AstArena *arena, int line, AstNode *condition, AstNode *body);
AstNode *csAstFor(AstArena *arena, int line, AstNode *initializer, AstNode *condition,
                  AstNode *increment, AstNode *body);
AstNode *csAstFunction(AstArena *arena, int line, const char *name, int nameLength);
void csAstFunctionAddParam(AstArena *arena, AstNode *function, const char *name,
                           int length, TypeKind type, bool hasAnnotation);
AstNode *csAstConditional(AstArena *arena, int line, AstNode *condition,
                          AstNode *thenValue, AstNode *elseValue);
AstNode *csAstBreak(AstArena *arena, int line);
AstNode *csAstContinue(AstArena *arena, int line);
AstNode *csAstSwitch(AstArena *arena, int line, AstNode *subject);
void csAstSwitchAddCase(AstArena *arena, AstNode *node, AstNode *test, AstNode *body);
AstNode *csAstIndex(AstArena *arena, int line, AstNode *target, AstNode *index);
AstNode *csAstObjectLiteral(AstArena *arena, int line);
void csAstObjectLiteralAdd(AstArena *arena, AstNode *object, AstNode *key,
                           AstNode *value);
AstNode *csAstArrayLiteral(AstArena *arena, int line);
void csAstArrayLiteralAdd(AstArena *arena, AstNode *array, AstNode *element);
AstNode *csAstForOf(AstArena *arena, int line, const char *name, int nameLength,
                    bool isConst, AstNode *iterable, AstNode *body);
AstNode *csAstTry(AstArena *arena, int line, AstNode *body, const char *catchName,
                  int catchNameLength, AstNode *catchBody, AstNode *finallyBody);
AstNode *csAstThrow(AstArena *arena, int line, AstNode *thrown);
AstNode *csAstSpread(AstArena *arena, int line, AstNode *expression);
AstNode *csAstDestructure(AstArena *arena, int line, bool isObject, bool isConst);
void csAstDestructureAdd(AstArena *arena, AstNode *node, const char *key,
                         int keyLength, const char *name, int nameLength,
                         AstNode *defaultValue, bool isRest);
AstNode *csAstReturn(AstArena *arena, int line, AstNode *value);
AstNode *csAstProgram(AstArena *arena, int line);

/* Appends to a AST_PROGRAM or AST_BLOCK statement list. */
void csAstProgramAdd(AstArena *arena, AstNode *parent, AstNode *statement);

const char *csUnaryOpName(UnaryOp op);
const char *csBinaryOpName(BinaryOp op);
const char *csLogicalOpName(LogicalOp op);

#endif /* CSCRIPT_AST_H */
