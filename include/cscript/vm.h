/* vm.h — the stack machine that executes compiled chunks.
 *
 * There is one global VM. It owns every heap object, the intern pool and the
 * globals map, which is also what makes it the collector's root set.
 */
#ifndef CSCRIPT_VM_H
#define CSCRIPT_VM_H

#include "cscript/chunk.h"
#include "cscript/common.h"
#include "cscript/table.h"
#include "cscript/value.h"

typedef enum {
  CS_OK,
  CS_COMPILE_ERROR,
  CS_RUNTIME_ERROR,
} InterpretResult;

#define CS_TEMP_ROOTS_MAX 64

typedef struct {
  const Chunk *chunk;
  const uint8_t *ip; /* next instruction to read */

  Value stack[CS_STACK_MAX];
  Value *stackTop;

  Table globals;
  /* Names bound by `const` or by a built-in. Used as a set; the values are
   * ignored. Assigning to anything in here is a runtime error, which is what
   * keeps `console = 1` from silently destroying the console. */
  Table globalConsts;
  Table strings; /* intern pool; weak — swept entries are removed */

  Obj *objects; /* head of the intrusive list of every live object */

  /* Tri-colour marking worklist. Deliberately allocated with raw realloc so a
   * collection can never recurse into itself. */
  int grayCount;
  int grayCapacity;
  Obj **grayStack;

  size_t bytesAllocated;
  size_t nextGC;

  Obj *tempRoots[CS_TEMP_ROOTS_MAX];
  int tempRootCount;

  const char *sourceName;
} VM;

extern VM vm;

void csVMInit(void);
void csVMFree(void);

/* Compiles and runs a whole source string. */
InterpretResult csInterpret(const char *source, const char *sourceName);

/* Marks a global as constant. Used by `const` and by the built-ins. */
void csVMMarkGlobalConst(ObjString *name);

void csVMPush(Value value);
Value csVMPop(void);

/* Reports a runtime error at the currently executing instruction and unwinds.
 * Native functions call this before returning false. */
void csVMRuntimeError(const char *format, ...);

#endif /* CSCRIPT_VM_H */
