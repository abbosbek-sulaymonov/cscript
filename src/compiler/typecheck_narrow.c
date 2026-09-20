/* typecheck_narrow.c — what a test proves about the thing it tested.
 *
 * `unknown` and a union are both types a program cannot use until it has said
 * what is inside, and this is where that saying is understood: `typeof x ===
 * "number"`, `x !== null`, `Array.isArray(x)`, and the `&&` and `||` chains
 * those get written in.
 *
 * The rules are deliberately a short list of *shapes* rather than a dataflow
 * analysis. Each one is a sentence a reader can hold: this test, in this
 * branch, proves this much. A guard that leaves — `if (typeof x !== "number")
 * return;` — proves it for the rest of the block, which is the one control
 * flow fact worth knowing, and the reason alwaysLeaves is here.
 */
#include <string.h>

#include "compiler/typecheck_internal.h"

/* Does this statement always leave — so that whatever follows it in the block
 * only runs when the branch was not taken?
 *
 * `throw`, `return`, `break` and `continue`, and a block whose last statement
 * is one of those. Deliberately shallow: it is the shape a guard is written
 * in, and recognising exactly that shape keeps the rule one sentence long. */
static bool alwaysLeaves(const AstNode *node) {
  if (node == NULL) return false;
  if (node->type == AST_RETURN_STMT || node->type == AST_THROW_STMT || node->type == AST_BREAK_STMT || node->type == AST_CONTINUE_STMT) {
    return true;
  }
  if (node->type != AST_BLOCK) return false;
  int count = node->as.block.count;
  return count > 0 && alwaysLeaves(node->as.block.statements[count - 1]);
}

bool csTypeBranchAlwaysLeaves(const AstNode *node) {
  return alwaysLeaves(node);
}

/* Every narrowing a condition carries, not only the first.
 *
 * `typeof a !== "object" || typeof b !== "object"` being false proves *both*
 * halves false, and a guard written that way — which is how a two-argument
 * function checks its arguments — should prove both. Answers how many were
 * recorded, so a caller can put them all back. */
int csTypeNarrowAll(Checker *checker, AstNode *condition, bool whenTrue, Variable **narrowed, TypeId *saved, int limit) {
  if (condition == NULL || limit <= 0) return 0;

  if (condition->type == AST_GROUPING) {
    return csTypeNarrowAll(checker, condition->as.grouping, whenTrue, narrowed, saved, limit);
  }

  if (condition->type == AST_UNARY && condition->as.unary.op == UNARY_NOT) {
    return csTypeNarrowAll(checker, condition->as.unary.operand, !whenTrue, narrowed, saved, limit);
  }

  if (condition->type == AST_LOGICAL) {
    bool carries = condition->as.logical.op == LOGICAL_AND ? whenTrue : !whenTrue;
    if (!carries) return 0;
    int count = csTypeNarrowAll(checker, condition->as.logical.left, whenTrue, narrowed, saved, limit);
    count += csTypeNarrowAll(checker, condition->as.logical.right, whenTrue, narrowed + count, saved + count, limit - count);
    return count;
  }

  TypeId was = TYPE_DYNAMIC;
  Variable *one = csTypeNarrow(checker, condition, whenTrue, &was);
  if (one == NULL) return 0;
  narrowed[0] = one;
  saved[0] = was;
  return 1;
}

/* Is this condition `typeof x === "name"`, and if so what does it prove?
 *
 * Deliberately the one shape rather than a general flow analysis. It is the
 * shape a program actually writes to check a `value`, and recognising exactly
 * it means the rule a reader has to know is one line long. `!==` proves the
 * same thing about the *other* branch, which is why `whenTrue` is a parameter
 * rather than the caller inverting anything. */
/* What is left of a type once a test has said something about it.
 *
 * For a union this is a filter: `string | null` asked `typeof x === "string"`
 * is a string, and asked `x !== null` is a string as well. For anything else
 * the test either fits or the program has contradicted itself, and taking the
 * proved type is what makes the branch checkable either way.
 *
 * `keep` says which side of the test this is: true for the branch where the
 * members that match survive, false for the branch where they are the ones
 * removed. */
static TypeId narrowedTo(Checker *checker, TypeId current, TypeId proved, bool keep) {
  const CompositeType *union_ = csTypeComposite(checker->types, current);
  if (union_ == NULL || union_->kind != COMPOSITE_UNION) {
    if (keep) return proved;
    /* Removing the one thing it could be leaves nothing to say. */
    return current == proved ? TYPE_DYNAMIC : current;
  }

  TypeId kept[16];
  int count = 0;
  for (int i = 0; i < union_->slotCount && count < (int)(sizeof kept / sizeof kept[0]); i++) {
    TypeId member = checker->types->slots[union_->slotStart + i];
    /* `typeof x === "object"` is true of an interface and of an array as well
     * as of a plain object, which is what makes the test worth writing. */
    bool matches = member == proved || csTypeAssignableIn(checker->types, member, proved);
    if (matches == keep) kept[count++] = member;
  }
  if (count == 0) return keep ? proved : TYPE_DYNAMIC;
  return csTypeUnionOf(checker->types, kept, count);
}

Variable *csTypeNarrow(Checker *checker, AstNode *condition, bool whenTrue, TypeId *saved) {
  if (condition == NULL) return NULL;

  /* `(…)` is not part of the shape. */
  if (condition->type == AST_GROUPING) {
    return csTypeNarrow(checker, condition->as.grouping, whenTrue, saved);
  }

  /* `!test` proves about its operand exactly what `test` proves about the
   * other branch. Which is what `if (!isPoint(v)) return;` rests on — the
   * guard shape a program actually writes — and it was not understood for
   * `typeof` either. */
  if (condition->type == AST_UNARY && condition->as.unary.op == UNARY_NOT) {
    return csTypeNarrow(checker, condition->as.unary.operand, !whenTrue, saved);
  }

  /* A guard is usually more than one test: `typeof x !== "number" || x < 0`.
   * The first operand of an `||` is proved false when the whole thing is, and
   * the first operand of an `&&` is proved true when the whole thing is — so
   * the same recognition applies to it, and nothing else in the chain can be
   * relied on either way. */
  if (condition->type == AST_LOGICAL) {
    bool carries = condition->as.logical.op == LOGICAL_AND ? whenTrue : !whenTrue;
    if (!carries) return NULL;
    /* Only the left here — the caller that wants every term in the chain uses
     * csTypeNarrowAll, which walks both sides. */
    return csTypeNarrow(checker, condition->as.logical.left, whenTrue, saved);
  }

  /* `Array.isArray(x)` is the other question a program asks about a value it
   * has been handed, and the answer is exactly a type. Recognised here for the
   * same reason `typeof` is: it is a built-in whose contract the checker
   * already knows, so trusting it costs nothing and pretending not to
   * understand it would push every caller into a cast the language does not
   * have. */
  if (condition->type == AST_CALL && whenTrue) {
    AstNode *callee = condition->as.call.callee;
    if (callee != NULL && callee->type == AST_PROPERTY && callee->as.property.length == 7 && memcmp(callee->as.property.name, "isArray", 7) == 0 &&
        callee->as.property.object != NULL && callee->as.property.object->type == AST_IDENTIFIER && callee->as.property.object->as.identifier.length == 5 &&
        memcmp(callee->as.property.object->as.identifier.name, "Array", 5) == 0 && condition->as.call.argCount == 1 &&
        condition->as.call.arguments[0]->type == AST_IDENTIFIER) {
      AstNode *subject = condition->as.call.arguments[0];
      Variable *variable = csTypeFindVariable(checker, subject->as.identifier.name, subject->as.identifier.length);
      if (variable == NULL) return NULL;
      *saved = variable->type;
      variable->type = TYPE_ARRAY;
      return variable;
    }

    /* `isText(value)` where `isText` was declared `(x: unknown): x is string`.
     * A predicate is the program writing down the same kind of contract
     * `Array.isArray` has built in — so it is trusted the same way, and that
     * is the whole of why one is worth writing.
     *
     * Only the true branch says anything. A false answer means the value is
     * not that type, which this lattice cannot subtract in general: what is
     * left of `unknown` after removing `string` has no name here. */
    if (callee != NULL && callee->type == AST_IDENTIFIER) {
      Variable *named = csTypeFindVariable(checker, callee->as.identifier.name, callee->as.identifier.length);
      const Signature *signature = named != NULL ? named->signature : NULL;
      if (signature == NULL || signature->predicateParam < 0) return NULL;
      if (signature->predicateParam >= condition->as.call.argCount) return NULL;

      AstNode *subject = condition->as.call.arguments[signature->predicateParam];
      if (subject == NULL || subject->type != AST_IDENTIFIER) return NULL;

      Variable *variable = csTypeFindVariable(checker, subject->as.identifier.name, subject->as.identifier.length);
      if (variable == NULL || variable->awaiting) return NULL;
      *saved = variable->type;
      variable->type = signature->predicateType;
      return variable;
    }
    return NULL;
  }

  if (condition->type != AST_BINARY) return NULL;

  BinaryOp op = condition->as.binary.op;
  bool equality = op == BINARY_EQUAL;
  bool inequality = op == BINARY_NOT_EQUAL;
  if (!equality && !inequality) return NULL;

  AstNode *left = condition->as.binary.left;
  AstNode *right = condition->as.binary.right;
  if (left == NULL || right == NULL) return NULL;

  /* `x === null` and `x !== undefined` — the test a union of "something, or
   * nothing" exists to be asked. Both branches say something here, unlike the
   * typeof test below: one keeps the absent case and the other removes it. */
  if (left->type == AST_IDENTIFIER && (right->type == AST_NULL_LITERAL || right->type == AST_UNDEFINED_LITERAL)) {
    Variable *variable = csTypeFindVariable(checker, left->as.identifier.name, left->as.identifier.length);
    if (variable == NULL) return NULL;
    /* A variable still waiting for its first assignment has no type to make
     * more specific, and narrowing one would undo the assignment the branch is
     * usually there to make: `if (table === null) table = build();` is the
     * shape, and what it settles has to survive the `if`. */
    if (variable->awaiting) return NULL;
    TypeId absent = right->type == AST_NULL_LITERAL ? TYPE_NULL : TYPE_UNDEFINED;
    *saved = variable->type;
    variable->type = narrowedTo(checker, variable->type, absent, whenTrue == equality);
    return variable;
  }

  /* `role === "admin"` — a set of names is only a type worth having if asking
   * which one narrows it. Both branches say something: the one the test
   * selects keeps that name, and the other drops it, which is what makes a
   * chain of them exhaustive. */
  if (left->type == AST_IDENTIFIER && (right->type == AST_STRING_LITERAL || right->type == AST_NUMBER_LITERAL)) {
    Variable *variable = csTypeFindVariable(checker, left->as.identifier.name, left->as.identifier.length);
    if (variable == NULL || variable->awaiting) return NULL;
    TypeId literal = right->type == AST_STRING_LITERAL ? csTypeLiteral(checker->types, right->as.string.chars, right->as.string.length)
                                                       : csTypeNumberLiteral(checker->types, right->as.number);
    if (!csTypeIs(checker->types, variable->type, COMPOSITE_UNION) && !csTypeIs(checker->types, variable->type, COMPOSITE_LITERAL) &&
        !csTypeIs(checker->types, variable->type, COMPOSITE_NUMBER_LITERAL)) {
      return NULL;
    }
    *saved = variable->type;
    variable->type = narrowedTo(checker, variable->type, literal, whenTrue == equality);
    return variable;
  }

  if (left->type != AST_UNARY || left->as.unary.op != UNARY_TYPEOF) return NULL;
  if (right->type != AST_STRING_LITERAL) return NULL;

  AstNode *subject = left->as.unary.operand;
  if (subject == NULL || subject->type != AST_IDENTIFIER) return NULL;

  TypeId proved;
  if (!csTypeFromTypeofName(right->as.string.chars, right->as.string.length, &proved)) {
    return NULL;
  }

  Variable *variable = csTypeFindVariable(checker, subject->as.identifier.name, subject->as.identifier.length);
  if (variable == NULL) return NULL;

  /* Narrowing only ever makes a type more specific: a union keeps the members
   * that fit, and anything else takes the proved type. A contradiction —
   * `typeof n === "string"` where n is a number — is the program's mistake to
   * make rather than this function's to silently accept. */
  /* Both branches of a typeof test say something: the one the test selects
   * keeps what matches, and the other keeps what does not. That second half is
   * what makes an `else` on a union usable — `typeof x === "number"` failing
   * leaves a string, and the branch can call a string's methods. */
  *saved = variable->type;
  variable->type = narrowedTo(checker, variable->type, proved, whenTrue == equality);
  return variable;
}
