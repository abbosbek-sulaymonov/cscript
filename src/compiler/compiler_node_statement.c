/* compiler_node_statement.c — the statements, from the compiler's node dispatcher.
 *
 * A statement leaves the stack as it found it. Most of these are one line
 * handing off to compiler_statement.c or compiler_class.c; what is here is the
 * ones whose whole job is the jump arithmetic — `break` and `continue` have to
 * know how many scopes they are leaving, and `switch` has to patch a chain of
 * them. Answers false for a node it does not handle.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler/compiler_internal.h"

bool compileStatementNode(const AstNode *node, int line) {
  switch (node->type) {
    case AST_EXPRESSION_STMT:
      /* A statement's value is discarded, which leaves the stack balanced. */
      compileForEffect(node->as.expression);
      break;

    case AST_VAR_DECL: compileVarDecl(node); break;

    case AST_BLOCK:
      beginScope();
      compileStatements(node->as.block.statements, node->as.block.count);
      endScope(line);
      break;

    case AST_IF_STMT: compileIf(node); break;

    case AST_WHILE_STMT: compileWhile(node); break;

    case AST_FOR_STMT: compileFor(node); break;

    case AST_FOR_OF_STMT: compileForOf(node); break;

    case AST_FUNCTION:
      /* A declaration binds the closure to its name; an expression leaves it
       * on the stack for whatever wanted it. An inferred name is not a
       * declaration — the binding it was named after does its own.
       *
       * A local declaration names its slot *before* the body is compiled, so
       * that the body can resolve its own name: the slot is where OP_CLOSURE
       * is about to push, and a recursive call reads it through an upvalue
       * that is captured open and therefore sees the closure once it lands
       * there. Declaring afterwards — which is the ordinary order, since a
       * value is normally already in the slot — left a local function unable
       * to call itself. */
      if (node->as.function.isDeclaration && current->scopeDepth > 0) {
        addLocal(node->as.function.name, node->as.function.nameLength, false, line);
        compileFunction(node);
        break;
      }

      compileFunction(node);
      if (node->as.function.isDeclaration) {
        addGlobal(node->as.function.name, node->as.function.nameLength, false, line);
        emitConstantOp(OP_DEFINE_GLOBAL, identifierConstant(node->as.function.name, node->as.function.nameLength, line), line);
      }
      break;

    case AST_BREAK_STMT: {
      Loop *target = compilerTargetLoop(node->as.jump.label, node->as.jump.labelLength, false, line);
      if (target == NULL) break;
      if (target->breakCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'break' statements in one loop (limit %d)", MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(target->scopeDepth, line);
      discardLocalsAbove(target->scopeDepth, line);
      target->breakJumps[target->breakCount++] = emitJump(OP_JUMP, line);
      break;
    }

    case AST_CONTINUE_STMT: {
      Loop *target = compilerTargetLoop(node->as.jump.label, node->as.jump.labelLength, true, line);
      if (target == NULL) break;
      if (target->continueCount >= MAX_LOOP_EXITS) {
        errorAt(line, "too many 'continue' statements in one loop (limit %d)", MAX_LOOP_EXITS);
        break;
      }
      unwindTryBlocks(target->scopeDepth, line);
      discardLocalsAbove(target->scopeDepth, line);
      target->continueJumps[target->continueCount++] = emitJump(OP_JUMP, line);
      break;
    }

    case AST_LABELED_STMT: {
      const AstNode *body = node->as.labeled.body;

      /* A label on a loop or a switch belongs to that construct, so it is
       * handed over for beginLoop to adopt — which is what lets `continue
       * outer` reach the right increment. */
      if (body->type == AST_WHILE_STMT || body->type == AST_FOR_STMT || body->type == AST_FOR_OF_STMT || body->type == AST_SWITCH_STMT) {
        pendingLabel = node->as.labeled.name;
        pendingLabelLength = node->as.labeled.length;
        compileNode(body);
        pendingLabel = NULL;
        pendingLabelLength = 0;
        break;
      }

      /* `outer: { … break outer; }` — a labelled block is a jump target and
       * nothing more, so it gets a context that catches `break` and refuses
       * `continue`, exactly as a switch does. */
      Loop loop;
      pendingLabel = node->as.labeled.name;
      pendingLabelLength = node->as.labeled.length;
      beginLoop(&loop, false);
      compileNode(body);
      endLoop(&loop, line);
      break;
    }

    case AST_SWITCH_STMT: {
      compileNode(node->as.switchStmt.subject);

      Loop loop;
      /* A switch catches `break` but not `continue`, which belongs to any
       * enclosing loop. */
      beginLoop(&loop, false);

      int bodyJumps[MAX_LOOP_EXITS];
      int bodyCount = 0;
      int nextTest = -1;

      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        if (nextTest != -1) patchJump(nextTest, line);

        /* Compare against a copy so the subject survives for the next arm. */
        emitByte(OP_DUP, line);
        compileNode(node->as.switchStmt.cases[i].test);
        emitByte(OP_EQUAL, line);
        nextTest = emitJump(OP_POP_JUMP_IF_FALSE, line);

        if (bodyCount < MAX_LOOP_EXITS) {
          bodyJumps[bodyCount++] = emitJump(OP_JUMP, line);
        }
      }
      if (nextTest != -1) patchJump(nextTest, line);

      /* Nothing matched: fall into `default` if there is one. */
      int afterDefault = -1;
      emitByte(OP_POP, line); /* the subject */
      if (node->as.switchStmt.defaultBody != NULL) {
        beginScope();
        compileStatements(node->as.switchStmt.defaultBody->as.block.statements, node->as.switchStmt.defaultBody->as.block.count);
        endScope(line);
      }
      afterDefault = emitJump(OP_JUMP, line);

      for (int i = 0; i < node->as.switchStmt.caseCount && i < bodyCount; i++) {
        patchJump(bodyJumps[i], line);
        emitByte(OP_POP, line); /* the subject */
        beginScope();
        compileStatements(node->as.switchStmt.cases[i].body->as.block.statements, node->as.switchStmt.cases[i].body->as.block.count);
        endScope(line);
        /* Arms do not fall through, so each one jumps to the end. */
        if (loop.breakCount < MAX_LOOP_EXITS) {
          loop.breakJumps[loop.breakCount++] = emitJump(OP_JUMP, line);
        }
      }

      patchJump(afterDefault, line);
      endLoop(&loop, line);
      break;
    }

    case AST_TRY_STMT: compileTry(node); break;

    case AST_THROW_STMT:
      compileNode(node->as.thrown);
      emitByte(OP_THROW, line);
      break;

    case AST_RETURN_STMT:
      if (current->kind == FUNCTION_SCRIPT) {
        errorAt(line, "'return' outside of a function");
        break;
      }
      if (current->kind == FUNCTION_CONSTRUCTOR && node->as.returnValue != NULL) {
        errorAt(line,
                "a constructor cannot return a value; it always yields "
                "the new instance");
        break;
      }
      /* The value is computed first so the finally blocks run with it already
       * on the stack — they are balanced, so it survives them. */
      if (node->as.returnValue != NULL) {
        compileNode(node->as.returnValue);
      } else if (current->kind == FUNCTION_CONSTRUCTOR) {
        emitBytes(OP_GET_LOCAL, 0, line);
      } else {
        emitByte(OP_UNDEFINED, line);
      }
      unwindTryBlocks(-1, line);
      emitByte(OP_RETURN, line);
      break;

    case AST_CLASS_DECL: compileClassDecl(node); break;

    case AST_IMPORT: compileImport(node); break;

    case AST_EXPORT: compileExport(node); break;

    case AST_PROGRAM: compileStatements(node->as.program.statements, node->as.program.count); break;

    default: return false;
  }
  return true;
}
