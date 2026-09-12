/* compiler_expression.c — operators, and the conditions that feed a jump.
 *
 * The one thing every function here maintains is the stack discipline: an
 * expression leaves exactly one value, and an operator consumes exactly what
 * it was promised. Where an operand's type is known the opcode is the
 * specialised one — OP_ADD_NUM rather than OP_ADD — which is where the type
 * checker's work stops being advice and starts being code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler/compiler_internal.h"

/* Emits a binary operator's two operands, fusing "a local, then a literal" into
 * one instruction. That shape covers `i < n`, `i % 7` and `total + 1`, and
 * profiling put it at 14-18% of everything executed in loop-heavy code. */
void compileOperandPair(const AstNode *left, const AstNode *right, int line) {
  if (left->type == AST_IDENTIFIER && right->type == AST_NUMBER_LITERAL) {
    int slot = resolveLocal(current, left->as.identifier.name, left->as.identifier.length);
    if (slot != -1) {
      emitByte(OP_GET_LOCAL_CONST, line);
      emitByte((uint8_t)slot, line);
      emitConstantOperand(makeConstant(NUMBER_VAL(right->as.number), line), line);
      return;
    }
  }

  if (left->type == AST_IDENTIFIER && right->type == AST_IDENTIFIER) {
    int leftSlot = resolveLocal(current, left->as.identifier.name, left->as.identifier.length);
    int rightSlot = resolveLocal(current, right->as.identifier.name, right->as.identifier.length);
    if (leftSlot != -1 && rightSlot != -1) {
      emitByte(OP_GET_LOCAL_LOCAL, line);
      emitByte((uint8_t)leftSlot, line);
      emitByte((uint8_t)rightSlot, line);
      return;
    }
  }

  compileNode(left);
  compileNode(right);
}

/* The instruction a binary operator compiles to, ignoring specialisation.
 * Shared with compound assignment, which builds the same operation without an
 * AST_BINARY node to hang resolved types off. */
uint8_t binaryOpcode(BinaryOp op) {
  switch (op) {
    case BINARY_ADD: return OP_ADD;
    case BINARY_SUBTRACT: return OP_SUBTRACT;
    case BINARY_MULTIPLY: return OP_MULTIPLY;
    case BINARY_DIVIDE: return OP_DIVIDE;
    case BINARY_MODULO: return OP_MODULO;
    case BINARY_EXPONENT: return OP_EXPONENT;
    case BINARY_EQUAL: return OP_EQUAL;
    case BINARY_NOT_EQUAL: return OP_NOT_EQUAL;
    case BINARY_GREATER: return OP_GREATER;
    case BINARY_GREATER_EQUAL: return OP_GREATER_EQUAL;
    case BINARY_LESS: return OP_LESS;
    case BINARY_LESS_EQUAL: return OP_LESS_EQUAL;
    case BINARY_INSTANCEOF: return OP_INSTANCEOF;
    case BINARY_IN: return OP_IN;
  }
  return OP_ADD;
}

void compileBinary(const AstNode *node) {
  compileOperandPair(node->as.binary.left, node->as.binary.right, node->line);

  int line = node->line;

  /* Where the checker resolved both sides to `number`, the generic OP_ADD's
   * string test is dead weight. This is the hook the rest of the specialisation
   * work hangs off: the types are consumed, not erased. */
  bool bothNumbers = node->as.binary.left->resolvedType == TYPE_NUMBER && node->as.binary.right->resolvedType == TYPE_NUMBER;

  /* Recorded for tiering: an operation whose operand types are known is one a
   * compiler could emit unboxed, with no guard and nothing to deoptimise to.
   * The ratio across a hot function is what decides whether a type-directed
   * backend is worth building — see jit.h. */
  if (bothNumbers) {
    current->function->typedSites++;
  } else {
    current->function->genericSites++;
  }

  emitByte(node->as.binary.op == BINARY_ADD && bothNumbers ? OP_ADD_NUM : binaryOpcode(node->as.binary.op), line);
}

/* Optional chaining.
 *
 * `a?.b.c()` short-circuits the *chain*, not the link: a nullish `a` skips the
 * `.c` and the call as well. So every `?.` in one chain jumps to the same
 * place, and only the outermost expression knows where that is — which is why
 * the parser wraps the chain in a node of its own.
 *
 * Chains nest only as expressions nest (`a?.b(c?.d)`), so a small fixed stack
 * is enough; nothing a program can grow feeds it. */
#define CS_MAX_CHAIN_DEPTH 16
#define CS_MAX_CHAIN_LINKS 32

typedef struct {
  int jumps[CS_MAX_CHAIN_LINKS];
  int count;
} OptionalChain;

static OptionalChain chains[CS_MAX_CHAIN_DEPTH];
static int chainDepth = 0;

/* Emitted with the value that `?.` tests on top of the stack, and nothing of
 * the chain's below it — which is what lets the landing site replace exactly
 * one value regardless of which link jumped. */
void emitOptionalGuard(int line) {
  if (chainDepth == 0) return;
  OptionalChain *chain = &chains[chainDepth - 1];
  if (chain->count >= CS_MAX_CHAIN_LINKS) {
    errorAt(line, "too many '?.' links in one chain (limit %d)", CS_MAX_CHAIN_LINKS);
    return;
  }
  chain->jumps[chain->count++] = emitJump(OP_JUMP_IF_NULLISH, line);
}

/* An unconditional jump into the same landing site. Used where a link has to
 * tidy the stack before it short-circuits. */
void emitOptionalJump(int line) {
  if (chainDepth == 0) return;
  OptionalChain *chain = &chains[chainDepth - 1];
  if (chain->count >= CS_MAX_CHAIN_LINKS) {
    errorAt(line, "too many '?.' links in one chain (limit %d)", CS_MAX_CHAIN_LINKS);
    return;
  }
  chain->jumps[chain->count++] = emitJump(OP_JUMP, line);
}

void compileOptionalChain(const AstNode *node) {
  int line = node->line;
  if (chainDepth >= CS_MAX_CHAIN_DEPTH) {
    errorAt(line, "'?.' chains nested too deeply (limit %d)", CS_MAX_CHAIN_DEPTH);
    return;
  }

  chains[chainDepth++].count = 0;
  compileNode(node->as.expression);
  OptionalChain *chain = &chains[--chainDepth];
  if (chain->count == 0) return;

  int over = emitJump(OP_JUMP, line);
  for (int i = 0; i < chain->count; i++) patchJump(chain->jumps[i], line);
  /* The result of a short-circuit is undefined even when the value that
   * caused it was null: `null?.x` is undefined, which is observable. */
  emitByte(OP_POP, line);
  emitByte(OP_UNDEFINED, line);
  patchJump(over, line);
}

/* OP_AWAIT and the note that this body needs a fiber to run on.
 *
 * The two always go together: `await` suspends a fiber, and the top level of a
 * file only gets one when it is marked. Emitting the instruction anywhere
 * without the mark produces a body that compiles and cannot run. */
void emitAwait(int line) {
  emitByte(OP_AWAIT, line);
  if (current->kind == FUNCTION_SCRIPT) current->function->isAsync = true;
}

/* && and || evaluate to an operand, not to a boolean, so they compile to a
 * conditional jump that leaves the left value on the stack. */
void compileLogical(const AstNode *node) {
  int line = node->line;
  compileNode(node->as.logical.left);

  /* `??` asks a different question from `||`: whether the left side is
   * *present*, not whether it is truthy. `0 ?? 1` is 0. */
  uint8_t jumpOp = node->as.logical.op == LOGICAL_AND ? OP_JUMP_IF_FALSE : node->as.logical.op == LOGICAL_OR ? OP_JUMP_IF_TRUE : OP_JUMP_IF_NOT_NULLISH;
  int endJump = emitJump(jumpOp, line);

  /* Not short-circuiting: drop the left value, the right one is the result. */
  emitByte(OP_POP, line);
  compileNode(node->as.logical.right);
  patchJump(endJump, line);
}

void compileIdentifierLoad(const char *name, int length, int line) {
  if (isPrivateName(name, length)) {
    errorAt(line,
            "'%.*s' is a private name, which is only valid as a property "
            "inside the class that declares it",
            length, name);
    return;
  }

  int slot = resolveLocal(current, name, length);
  if (slot != -1) {
    emitBytes(OP_GET_LOCAL, (uint8_t)slot, line);
    return;
  }

  int upvalue = resolveUpvalue(current, name, length, line);
  if (upvalue != -1) {
    emitBytes(OP_GET_UPVALUE, (uint8_t)upvalue, line);
    return;
  }

  emitGlobalOp(OP_GET_GLOBAL, identifierConstant(name, length, line), line);
}

/* Maps a comparison to the fused jump that tests it directly. The jump is
 * taken when the comparison is false, so each opcode is its negation. */
bool fusedConditionJump(const AstNode *condition, uint8_t *opcode) {
  if (condition == NULL || condition->type != AST_BINARY) return false;

  switch (condition->as.binary.op) {
    case BINARY_LESS: *opcode = OP_JUMP_IF_NOT_LESS; return true;
    case BINARY_LESS_EQUAL: *opcode = OP_JUMP_IF_NOT_LESS_EQUAL; return true;
    case BINARY_GREATER: *opcode = OP_JUMP_IF_NOT_GREATER; return true;
    case BINARY_GREATER_EQUAL: *opcode = OP_JUMP_IF_NOT_GREATER_EQUAL; return true;
    case BINARY_EQUAL: *opcode = OP_JUMP_IF_NOT_EQUAL; return true;
    case BINARY_NOT_EQUAL: *opcode = OP_JUMP_IF_EQUAL; return true;
    default: return false;
  }
}

/* Emits a condition and the jump that skips the branch when it is false,
 * returning the offset to patch. A comparison compiles to a single fused
 * instruction rather than producing a boolean for the next one to consume. */
int emitConditionJump(const AstNode *condition, int line) {
  uint8_t fused;
  if (fusedConditionJump(condition, &fused)) {
    compileOperandPair(condition->as.binary.left, condition->as.binary.right, line);
    return emitJump(fused, line);
  }

  compileNode(condition);
  return emitJump(OP_POP_JUMP_IF_FALSE, line);
}

/* True when a subtree contains a function, which is the only way a program can
 * observe whether a loop variable is one binding or one per iteration.
 *
 * Used to decide whether a `for` loop needs the per-iteration copy below. A
 * loop with no closures in it keeps the single-slot form and stays fast. */
