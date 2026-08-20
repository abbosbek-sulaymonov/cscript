/* compiler_internal.h — the compiler's shared state and the seams between its
 * five translation units.
 *
 * Code generation is one pass over the AST, split by *what it compiles*:
 * expressions, statements, classes, modules, and the core the rest sit on —
 * the emit helpers, the scope and local machinery, and the dispatcher.
 *
 * The compiler keeps four pieces of ambient state, declared here and defined
 * in compiler.c. They are module-level rather than threaded through every call
 * because every one of them is genuinely ambient: which function is being
 * compiled, which unit it belongs to, which loop encloses the point being
 * compiled, and which `try` blocks a jump out of it would have to unwind.
 *
 * Nothing outside the compiler includes this. The public entry point is
 * csCompile in compiler.h.
 */
#ifndef CSCRIPT_COMPILER_INTERNAL_H
#define CSCRIPT_COMPILER_INTERNAL_H

#include "cscript/ast.h"
#include "cscript/chunk.h"
#include "cscript/compiler.h"
#include "cscript/diagnostic.h"
#include "cscript/object.h"

#define MAX_LOCALS 256

/* A variable visible in the current scope. Locals are resolved to a stack slot
 * at compile time, so reading one is an array index rather than a hash lookup —
 * which is the single biggest reason this is a compiler and not a tree walker. */
typedef struct {
  const char *name;
  int length;
  int depth;    /* scope nesting level it was declared at */
  bool isConst;
  bool isCaptured; /* a nested function closed over it */
} Local;

#define MAX_GLOBALS 256

/* A global declared by this compilation unit. Tracked so redeclaring a name is
 * a compile error rather than a silent overwrite, and so assigning to a const
 * is caught before the program ever runs. */
typedef struct {
  const char *name;
  int length;
  bool isConst;
} GlobalDecl;

/* Where a captured variable came from: a local of the immediately enclosing
 * function, or an upvalue that function had already captured itself. */
typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum {
  FUNCTION_SCRIPT, /* the implicit top-level function */
  FUNCTION_BODY,
  /* A method's slot 0 holds the receiver rather than the callee, and is named
   * `this` so an ordinary local lookup finds it — which also means an arrow
   * function inside a method captures it as an upvalue and gets JavaScript's
   * lexical `this` without any special rule. */
  FUNCTION_METHOD,
  FUNCTION_CONSTRUCTOR,
} FunctionKind;

static inline bool isMethodKind(FunctionKind kind) {
  return kind == FUNCTION_METHOD || kind == FUNCTION_CONSTRUCTOR;
}

/* One per function being compiled. They form a stack through `enclosing`, which
 * is what lets an inner function resolve a name to an outer function's local. */
typedef struct Compiler {
  struct Compiler *enclosing;
  ObjFunction *function;
  FunctionKind kind;

  Local locals[MAX_LOCALS];
  int localCount;
  int scopeDepth; /* 0 is the function's own top level */

  Upvalue upvalues[MAX_LOCALS];
} Compiler;

/* Globals are shared across every function in a compilation unit, so the
 * declaration table lives outside the per-function compiler. */
typedef struct {
  Diagnostics *diag;
  GlobalDecl globals[MAX_GLOBALS];
  int globalCount;
  /* The module being compiled. Every function it produces carries it, which is
   * how a global read at run time knows which scope to look in. */
  ObjModule *module;
} Unit;

#define MAX_LOOP_EXITS 64

/* The innermost enclosing loop or switch, so `break` and `continue` know where
 * to go. Jumps out are recorded here and patched once the exit point is known. */
typedef struct Loop {
  struct Loop *enclosing;
  int scopeDepth;      /* locals above this are discarded when jumping out */
  int continueTarget;  /* -1 while unknown, e.g. a for-loop's increment */
  bool allowsContinue; /* false inside a switch */

  int breakJumps[MAX_LOOP_EXITS];
  int breakCount;
  int continueJumps[MAX_LOOP_EXITS];
  int continueCount;
} Loop;

/* An enclosing `try` whose `finally` any jump out of it has to run first.
 *
 * `return`, `break` and `continue` all leave a try block without reaching the
 * end of its body, and JavaScript still runs the finally on each of those
 * paths. Tracking the open try blocks is what lets the compiler emit it. */
typedef struct TryContext {
  struct TryContext *enclosing;
  const AstNode *finallyBody; /* NULL for a try with only a catch */
  int scopeDepth;
  /* False while the catch block is being compiled: the handler was already
   * removed on entry to the catch, so jumping out of it must run the finally
   * but must not pop a handler that is no longer installed. */
  bool handlerActive;
} TryContext;



/* Ambient state, defined in compiler.c. */
extern Compiler *current;
extern Unit *currentUnit;
extern Loop *currentLoop;
extern TryContext *currentTry;


/* compiler.c — emit helpers, scopes, locals, and the node dispatcher */
Chunk *currentChunk(void);
void errorAt(int line, const char *format, ...);
void emitByte(uint8_t byte, int line);
void emitBytes(uint8_t a, uint8_t b, int line);
void emitConstantOperand(int index, int line);
void emitConstantOp(uint8_t opcode, int index, int line);
void emitPropertyOp(uint8_t opcode, int index, int line);
void emitGlobalOp(uint8_t opcode, int index, int line);
int makeConstant(Value value, int line);
void emitConstant(Value value, int line);
int identifierConstant(const char *name, int length, int line);
int emitJump(uint8_t instruction, int line);
int emitJump16(int line);
void patchJump(int offset, int line);
void emitLoop(int loopStart, int line);
void beginScope(void);
void endScope(int line);
bool identifiersEqual(const Local *local, const char *name, int length);
int resolveLocal(Compiler *compiler, const char *name, int length);
int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal, int line);
int resolveUpvalue(Compiler *compiler, const char *name, int length, int line);
bool enclosingLocalIsConst(Compiler *compiler, const char *name, int length);
void addLocal(const char *name, int length, bool isConst,
                     int line);
GlobalDecl *findGlobal(const char *name, int length);
void addGlobal(const char *name, int length, bool isConst,
                      int line);
void beginLoop(Loop *loop, bool allowsContinue);
void endLoop(Loop *loop, int line);
void compileStatements(AstNode *const *statements,
                              int count);
void beginFunction(Compiler *compiler, FunctionKind kind, const char *name,
                          int nameLength);
ObjFunction *endFunction(int line);
void emitClosure(const Compiler *compiler, ObjFunction *function, int line);
void compileNode(const AstNode *node);

/* src/compiler_expression.c */
void compileOperandPair(const AstNode *left, const AstNode *right, int line);
uint8_t binaryOpcode(BinaryOp op);
void compileBinary(const AstNode *node);
void compileLogical(const AstNode *node);
void compileOptionalChain(const AstNode *node);
void emitOptionalGuard(int line);
void emitOptionalJump(int line);
void compileIdentifierLoad(const char *name, int length, int line);
void compileAssign(const AstNode *node, bool discard);
void compileUpdate(const AstNode *node);
void compileForEffect(const AstNode *node);
bool fusedConditionJump(const AstNode *condition, uint8_t *opcode);
int emitConditionJump(const AstNode *condition, int line);
bool containsFunction(const AstNode *node);
void compileFunctionAs(const AstNode *node, FunctionKind kind);
void compileFunction(const AstNode *node);
bool compileThisLoad(int line);
bool compileSuperLoad(int line);

/* src/compiler_statement.c */
void compileIf(const AstNode *node);
void compileWhile(const AstNode *node);
void compileFor(const AstNode *node);
void compileForOf(const AstNode *node);
void compileTry(const AstNode *node);
void compileDestructurePattern(const AstNode *node, int line);
void compileDestructure(const AstNode *node);
void compileVarDecl(const AstNode *node);
void unwindTryBlocks(int stopAtDepth, int line);
void discardLocalsAbove(int depth, int line);

/* src/compiler_class.c */
int instanceFieldCount(const AstNode *node);
void emitFieldAssignments(const AstNode *node);
void compileFieldInitializer(const AstNode *node);
bool isSuperCallStatement(const AstNode *statement);
void compileConstructor(const AstNode *classNode);
void compileClassDecl(const AstNode *node);

/* src/compiler_module.c */
void defineModuleBinding(const char *name, int length, int line);
void compileImport(const AstNode *node);
void markExported(const char *name, int length, int line);
void compileExport(const AstNode *node);

#endif /* CSCRIPT_COMPILER_INTERNAL_H */
