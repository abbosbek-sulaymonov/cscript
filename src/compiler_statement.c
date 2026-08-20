/* compiler_statement.c — control flow and declarations.
 *
 * The four loop forms, `if`, `try`, and the destructuring that a declaration\n * lowers to. Everything here is about *where control goes* and *what is in\n * scope*, which is why the local-discarding and try-unwinding helpers live\n * here rather than in the core.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler_internal.h"


/* Emits the pops needed to leave every scope inside `depth` before jumping out
 * of a loop. The locals stay in the compiler's table, because the code after
 * the jump is still inside the loop and can still see them. */
void discardLocalsAbove(int depth, int line) {
  int count = 0;
  for (int i = current->localCount - 1; i >= 0; i--) {
    if (current->locals[i].depth <= depth) break;
    /* A captured local has to be closed individually, so the fast path is
     * abandoned as soon as one appears. */
    if (current->locals[i].isCaptured) {
      if (count == 1) {
        emitByte(OP_POP, line);
      } else if (count > 1) {
        emitBytes(OP_POP_N, (uint8_t)count, line);
      }
      count = 0;
      emitByte(OP_CLOSE_UPVALUE, line);
      continue;
    }
    count++;
  }

  if (count == 1) {
    emitByte(OP_POP, line);
  } else if (count > 1) {
    emitBytes(OP_POP_N, (uint8_t)count, line);
  }
}

void unwindTryBlocks(int stopAtDepth, int line) {
  for (TryContext *context = currentTry; context != NULL;
       context = context->enclosing) {
    if (context->scopeDepth <= stopAtDepth) break;
    if (context->handlerActive) emitByte(OP_END_TRY, line);
    if (context->finallyBody != NULL) compileNode(context->finallyBody);
  }
}

/* Destructures the value already on top of the stack.
 *
 * Split out from compileDestructure so a pattern can hold a pattern — the
 * piece a nested binding extracts is exactly the value this wants — and so a
 * parameter can be destructured from the slot it arrived in. */
void compileDestructurePattern(const AstNode *node, int line) {
  bool isObject = node->as.destructure.isObject;

  /* The source, held in a slot the body cannot name. */
  addLocal(" source", 7, true, line);
  int sourceSlot = current->localCount - 1;

  for (int i = 0; i < node->as.destructure.count; i++) {
    const AstBinding *binding = &node->as.destructure.bindings[i];

    if (binding->isRest) {
      emitBytes(OP_GET_LOCAL, (uint8_t)sourceSlot, line);
      if (isObject) {
        /* Everything the pattern already named, so the rest can exclude it.
         * A rest is always last, so those are exactly the first `i`. */
        for (int taken = 0; taken < i; taken++) {
          const AstBinding *named = &node->as.destructure.bindings[taken];
          emitConstantOp(OP_CONSTANT,
                         identifierConstant(named->key, named->keyLength, line), line);
        }
        emitBytes(OP_OBJECT_REST, (uint8_t)i, line);
      } else {
        emitBytes(OP_ARRAY_REST, (uint8_t)i, line);
      }
    } else {
      emitBytes(OP_GET_LOCAL, (uint8_t)sourceSlot, line);
      if (isObject) {
        emitPropertyOp(OP_GET_PROPERTY,
                       identifierConstant(binding->key, binding->keyLength, line),
                       line);
      } else {
        emitConstant(NUMBER_VAL(i), line);
        emitByte(OP_GET_INDEX, line);
      }

      /* A default applies when the piece is missing, which is what undefined
       * means for both a short array and an absent property. */
      if (binding->defaultValue != NULL) {
        emitByte(OP_DUP, line);
        emitByte(OP_UNDEFINED, line);
        emitByte(OP_NOT_EQUAL, line);
        int keepJump = emitJump(OP_POP_JUMP_IF_FALSE, line);
        int doneJump = emitJump(OP_JUMP, line);
        patchJump(keepJump, line);
        emitByte(OP_POP, line); /* the undefined */
        compileNode(binding->defaultValue);
        patchJump(doneJump, line);
      }
    }

    /* A nested pattern binds nothing itself: it destructures the piece just
     * extracted, which is already where that pattern's source belongs. */
    if (binding->pattern != NULL) {
      compileDestructurePattern(binding->pattern, line);
      continue;
    }

    /* The value is already sitting where the binding's slot will be. */
    if (current->scopeDepth > 0) {
      addLocal(binding->name, binding->nameLength, node->as.destructure.isConst, line);
    } else {
      addGlobal(binding->name, binding->nameLength, node->as.destructure.isConst, line);
      emitConstantOp(node->as.destructure.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
                     identifierConstant(binding->name, binding->nameLength, line), line);
    }
  }

  /* At global scope the hidden source local is the only thing still on the
   * stack; inside a block it stays put and the enclosing scope discards it. */
  if (current->scopeDepth == 0) {
    current->localCount--;
    emitByte(OP_POP, line);
  }
}

void compileDestructure(const AstNode *node) {
  compileNode(node->as.destructure.initializer);
  compileDestructurePattern(node, node->line);
}

void compileVarDecl(const AstNode *node) {
  int line = node->line;
  const char *name = node->as.varDecl.name;
  int length = node->as.varDecl.length;

  if (node->as.varDecl.initializer != NULL) {
    compileNode(node->as.varDecl.initializer);
  } else {
    emitByte(OP_UNDEFINED, line);
  }

  if (current->scopeDepth > 0) {
    /* The initialiser's value is already sitting in the slot the local will
     * occupy, so declaring it is pure bookkeeping — no instruction needed. */
    addLocal(name, length, node->as.varDecl.isConst, line);
    return;
  }

  addGlobal(name, length, node->as.varDecl.isConst, line);
  emitConstantOp(node->as.varDecl.isConst ? OP_DEFINE_CONST : OP_DEFINE_GLOBAL,
                 identifierConstant(name, length, line), line);
}

void compileIf(const AstNode *node) {
  int line = node->line;
  int thenJump = emitConditionJump(node->as.ifStmt.condition, line);
  compileNode(node->as.ifStmt.thenBranch);

  if (node->as.ifStmt.elseBranch == NULL) {
    patchJump(thenJump, line);
    return;
  }

  int elseJump = emitJump(OP_JUMP, line);
  patchJump(thenJump, line);
  compileNode(node->as.ifStmt.elseBranch);
  patchJump(elseJump, line);
}

void compileWhile(const AstNode *node) {
  int line = node->line;
  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  /* `do` runs the body before the first test, so the only difference is where
   * the condition sits: at the top, or at the bottom with no jump over it. */
  if (node->as.whileStmt.isDoWhile) {
    compileNode(node->as.whileStmt.body);

    /* `continue` in a do-while jumps to the test, not past it. */
    for (int i = 0; i < loop.continueCount; i++) {
      patchJump(loop.continueJumps[i], line);
    }
    int exitJump = emitConditionJump(node->as.whileStmt.condition, line);
    emitLoop(loopStart, line);
    patchJump(exitJump, line);
    endLoop(&loop, line);
    return;
  }

  int exitJump = emitConditionJump(node->as.whileStmt.condition, line);

  compileNode(node->as.whileStmt.body);

  /* `continue` re-tests the condition. */
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i], line);
  }
  emitLoop(loopStart, line);
  patchJump(exitJump, line);
  endLoop(&loop, line);
}

/* Desugars to a while loop, with the initialiser scoped to the loop so that
 * `for (let i = ...)` cannot leak `i` into the surrounding scope.
 *
 * When the body contains a closure, each iteration gets its *own* binding of
 * the loop variable — a shadowing copy made on entry and written back on exit.
 * JavaScript specifies that for `let`, and it is the difference between
 * collecting three closures that return 0, 1, 2 and three that all return 3.
 * The copy is skipped entirely when no closure can observe it. */
void compileFor(const AstNode *node) {
  int line = node->line;
  beginScope();

  if (node->as.forStmt.initializer != NULL) {
    compileNode(node->as.forStmt.initializer);
  }

  /* Per-iteration binding is only observable through a closure, so the copy is
   * emitted only when the loop actually contains one. */
  bool perIteration = node->as.forStmt.initializer != NULL &&
                      node->as.forStmt.initializer->type == AST_VAR_DECL &&
                      !node->as.forStmt.initializer->as.varDecl.isConst &&
                      containsFunction(node->as.forStmt.body);
  int outerSlot = perIteration ? current->localCount - 1 : -1;
  const char *bindingName =
      perIteration ? node->as.forStmt.initializer->as.varDecl.name : NULL;
  int bindingLength =
      perIteration ? node->as.forStmt.initializer->as.varDecl.length : 0;

  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  int exitJump = -1;
  if (node->as.forStmt.condition != NULL) {
    exitJump = emitConditionJump(node->as.forStmt.condition, line);
  }

  int innerSlot = -1;
  if (perIteration) {
    /* A shadowing copy, so the body — and anything it closes over — sees a
     * binding that belongs to this iteration alone. */
    beginScope();
    emitBytes(OP_GET_LOCAL, (uint8_t)outerSlot, line);
    addLocal(bindingName, bindingLength, false, line);
    innerSlot = current->localCount - 1;
  }

  compileNode(node->as.forStmt.body);

  /* `continue` in a for-loop must still run the increment, so it lands here
   * rather than at the condition — skipping it would spin forever. It also has
   * to reach the write-back, or the iteration's changes would be lost. */
  for (int i = 0; i < loop.continueCount; i++) {
    patchJump(loop.continueJumps[i], line);
  }

  if (perIteration) {
    /* Copy the iteration's value back before the shared slot is advanced. */
    emitBytes(OP_GET_LOCAL, (uint8_t)innerSlot, line);
    emitByte(OP_SET_LOCAL_POP, line);
    emitByte((uint8_t)outerSlot, line);
    endScope(line);
  }

  if (node->as.forStmt.increment != NULL) {
    compileForEffect(node->as.forStmt.increment);
  }

  emitLoop(loopStart, line);
  if (exitJump != -1) patchJump(exitJump, line);
  endLoop(&loop, line);

  endScope(line);
}

/* `for (const x of xs) body` becomes an index loop over two hidden locals: the
 * iterable itself and a counter. They are given names no source can produce, so
 * the body cannot see or shadow them.
 *
 * There is no iterator protocol yet — arrays and strings are the only iterable
 * things — so this stays a desugaring rather than a runtime mechanism. */
void compileForOf(const AstNode *node) {
  int line = node->line;
  beginScope();

  /* Slot 1: the iterable, evaluated once. `for...in` walks the same loop over
   * the key array, which is what the language means by enumerating an object. */
  compileNode(node->as.forOf.iterable);
  if (node->as.forOf.isForIn) {
    emitByte(OP_ENUM_KEYS, line);
  } else {
    emitByte(OP_ITER_PREPARE, line);
  }
  addLocal(" iterable", 9, true, line);
  int iterableSlot = current->localCount - 1;

  /* Slot 2: the index, starting at zero. */
  emitConstant(NUMBER_VAL(0), line);
  addLocal(" index", 6, false, line);
  int indexSlot = current->localCount - 1;

  int loopStart = currentChunk()->count;

  Loop loop;
  beginLoop(&loop, true);

  /* One step: the next element, or out. Asking for a length first would work
   * for an array and not for a generator, which has no length short of running
   * it to the end.
   *
   * `for await` has a second shape for the one thing an index cannot drive:
   * an async generator answers `next()` with a promise, so the loop must
   * await before it can know whether there is another value at all. Which
   * shape runs is decided per iteration by looking at the iterable, which
   * costs one type test and keeps the sync path exactly as it was. */
  int asyncPath = -1;
  emitBytes(OP_GET_LOCAL, (uint8_t)iterableSlot, line);
  if (node->as.forOf.isAwait) asyncPath = emitJump(OP_JUMP_IF_ASYNC_ITER, line);

  emitBytes(OP_GET_LOCAL, (uint8_t)indexSlot, line);
  int exitJump = emitJump(OP_ITER_STEP, line);

  /* `for await` awaits each element before the body sees it, which is what
   * makes a list of promises iterate as the values they settle to. */
  int asyncExit = -1;
  if (node->as.forOf.isAwait) {
    emitAwait(line);
    int boundJump = emitJump(OP_JUMP, line);

    patchJump(asyncPath, line);
    emitByte(OP_ASYNC_NEXT, line);
    emitAwait(line);
    asyncExit = emitJump(OP_ITER_UNPACK, line);

    patchJump(boundJump, line);
  }

  /* The binding is a fresh local per iteration, so a closure made in the body
   * captures that iteration's value rather than sharing one cell. The element
   * ITER_STEP pushed is already sitting where the local belongs. */
  beginScope();
  addLocal(node->as.forOf.name, node->as.forOf.nameLength, node->as.forOf.isConst,
           line);

  /* `for (const [k, v] of m)` — the element is in its slot; the pattern binds
   * the pieces beside it, fresh on every iteration. */
  if (node->as.forOf.pattern != NULL) {
    emitBytes(OP_GET_LOCAL, (uint8_t)(current->localCount - 1), line);
    compileDestructurePattern(node->as.forOf.pattern, line);
  }

  compileNode(node->as.forOf.body);
  endScope(line);

  for (int i = 0; i < loop.continueCount; i++) patchJump(loop.continueJumps[i], line);
  emitBytes(OP_INC_LOCAL, (uint8_t)indexSlot, line);
  emitLoop(loopStart, line);

  /* Both shapes leave the stack at the height the loop started at, so one
   * exit serves both: ITER_STEP drops the iterable and the index before it
   * jumps, and ITER_UNPACK drops the record. */
  patchJump(exitJump, line);
  if (asyncExit != -1) patchJump(asyncExit, line);
  endLoop(&loop, line);
  endScope(line);
}

/* try / catch / finally.
 *
 * The shape emitted is:
 *
 *     OP_TRY -> catch                install a handler
 *     <body>
 *     OP_END_TRY                     normal path: remove it
 *     <finally>                      run on the way out
 *     OP_JUMP -> end
 *   catch:                           the handler resumes here with the thrown
 *     <bind or discard the value>    value on top of the stack
 *     <catch body>
 *     <finally>
 *   end:
 *
 * `finally` is emitted twice rather than jumped to, because the two paths
 * arrive with different stacks and have to leave differently. Duplicating a
 * block is the cost of not needing a subroutine-return mechanism the VM does
 * not otherwise have.
 *
 * With no catch, the handler still points at the finally, which runs and then
 * rethrows so the exception keeps travelling. */
void compileTry(const AstNode *node) {
  int line = node->line;
  bool hasCatch = node->as.tryStmt.catchBody != NULL;
  bool hasFinally = node->as.tryStmt.finallyBody != NULL;

  int handlerJump = emitJump(OP_TRY, line);

  TryContext context;
  context.enclosing = currentTry;
  context.finallyBody = node->as.tryStmt.finallyBody;
  context.scopeDepth = current->scopeDepth;
  context.handlerActive = true;
  currentTry = &context;

  compileNode(node->as.tryStmt.body);

  currentTry = context.enclosing;
  emitByte(OP_END_TRY, line);

  if (hasFinally) compileNode(node->as.tryStmt.finallyBody);
  int endJump = emitJump(OP_JUMP, line);

  /* The handler resumes here with the thrown value on top of the stack. */
  patchJump(handlerJump, line);

  if (hasCatch) {
    /* The finally still covers the catch block, so a return out of it has to
     * run the finally — but the handler is already gone by this point. */
    context.handlerActive = false;
    currentTry = &context;

    beginScope();
    if (node->as.tryStmt.catchName != NULL) {
      /* The thrown value is already in the slot the binding will occupy. */
      addLocal(node->as.tryStmt.catchName, node->as.tryStmt.catchNameLength, false,
               line);
    } else {
      emitByte(OP_POP, line); /* `catch { }` ignores the value */
    }
    compileNode(node->as.tryStmt.catchBody);
    endScope(line);

    currentTry = context.enclosing;
    if (hasFinally) compileNode(node->as.tryStmt.finallyBody);
  } else {
    /* finally-only: run the block, then let the exception continue. */
    compileNode(node->as.tryStmt.finallyBody);
    emitByte(OP_THROW, line);
  }

  patchJump(endJump, line);
}
