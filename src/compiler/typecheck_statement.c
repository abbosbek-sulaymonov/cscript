/* typecheck_statement.c — the checking a statement needs.
 *
 * A statement has no type, so what these leave in `result` is incidental; what
 * they are for is the scope. A block opens one, a declaration adds to it, a
 * function body carries the return type every `return` in it is measured
 * against, and a loop carries whether `break` and `continue` mean anything
 * here.
 *
 * one of the two groups checkNode asks. Answers false for a node
 * belonging to the other, so a node neither claims keeps the `any` the checker
 * has always fallen back to. See typecheck.c for the walk and
 * typecheck_internal.h for the state they share.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/typecheck.h"

#include "compiler/typecheck_internal.h"

bool checkStatementNode(Checker *checker, AstNode *node, TypeKind *out) {
  TypeKind result = TYPE_ANY;

  switch (node->type) {
    case AST_LABELED_STMT:
      /* A label is a jump target; it declares nothing and types nothing. */
      checkNode(checker, node->as.labeled.body);
      result = TYPE_UNDEFINED;
      break;

    case AST_BREAK_STMT:
    case AST_CONTINUE_STMT:
      /* The compiler reports these when they are out of place, where it knows
       * the enclosing loop. */
      result = TYPE_UNDEFINED;
      break;

    case AST_SWITCH_STMT: {
      TypeKind subject = checkNode(checker, node->as.switchStmt.subject);
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        TypeKind test = checkNode(checker, node->as.switchStmt.cases[i].test);
        /* Arms are matched with ===, so an arm that can never match is the
         * same mistake as writing that comparison out by hand. */
        if (csTypeIsKnown(subject) && csTypeIsKnown(test) && subject != test) {
          csTypeError(checker, node->as.switchStmt.cases[i].test->line,
                    "this case is %s but the switch subject is %s, so it can never match",
                    csTypeName(test), csTypeName(subject));
        }
        checkNode(checker, node->as.switchStmt.cases[i].body);
      }
      checkNode(checker, node->as.switchStmt.defaultBody);
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_FUNCTION: {
      const Signature *signature = csTypeDeclareFunction(checker, node);
      csTypeCheckFunctionBody(checker, node, signature);
      result = TYPE_FUNCTION;
      break;
    }

    case AST_RETURN_STMT: {
      TypeKind returned = node->as.returnValue != NULL
                              ? checkNode(checker, node->as.returnValue)
                              : TYPE_UNDEFINED;
      if (checker->functionDepth == 0) {
        /* The compiler reports this with better placement. */
        result = TYPE_UNDEFINED;
        break;
      }
      if (checker->currentReturnAnnotated &&
          !csTypeAssignable(returned, checker->currentReturn)) {
        csTypeError(checker, node->line, "cannot return %s from a function declared %s",
                  csTypeName(returned), csTypeName(checker->currentReturn));
      }
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_VAR_DECL: {
      TypeKind initializer = node->as.varDecl.initializer != NULL
                                 ? checkNode(checker, node->as.varDecl.initializer)
                                 : TYPE_UNDEFINED;

      TypeKind declared;
      if (node->as.varDecl.hasAnnotation) {
        declared = node->as.varDecl.declaredType;
        if (!csTypeAssignable(initializer, declared)) {
          csTypeError(checker, node->line, "cannot assign %s to '%.*s', declared as %s",
                    csTypeName(initializer), node->as.varDecl.length,
                    node->as.varDecl.name, csTypeName(declared));
        }
      } else if (node->as.varDecl.initializer != NULL) {
        /* Inference: an unannotated declaration takes its initialiser's type,
         * which is what lets unannotated code still be checked.
         *
         * Except `null` and `undefined`, which say "nothing yet" rather than
         * name a type. `let x;` is already `any` and `let x = undefined;` is
         * the same declaration written out, so inferring from either would
         * make them disagree — and would make `x ??= 1` an error, which is
         * the case `??=` exists for. */
        declared = initializer == TYPE_NULL || initializer == TYPE_UNDEFINED
                       ? TYPE_ANY
                       : initializer;
      } else {
        declared = TYPE_ANY;
      }

      csTypeDeclareVariable(checker, node->as.varDecl.name, node->as.varDecl.length, declared);
      result = declared;
      break;
    }

    case AST_EXPRESSION_STMT:
      checkNode(checker, node->as.expression);
      result = TYPE_UNDEFINED;
      break;

    case AST_BLOCK:
      csTypeBeginScope(checker);
      for (int i = 0; i < node->as.block.count; i++) {
        checkNode(checker, node->as.block.statements[i]);
      }
      csTypeEndScope(checker);
      result = TYPE_UNDEFINED;
      break;

    case AST_IF_STMT:
      checkNode(checker, node->as.ifStmt.condition);
      checkNode(checker, node->as.ifStmt.thenBranch);
      checkNode(checker, node->as.ifStmt.elseBranch);
      result = TYPE_UNDEFINED;
      break;

    case AST_WHILE_STMT:
      checkNode(checker, node->as.whileStmt.condition);
      checkNode(checker, node->as.whileStmt.body);
      result = TYPE_UNDEFINED;
      break;

    case AST_TRY_STMT:
      checkNode(checker, node->as.tryStmt.body);
      if (node->as.tryStmt.catchBody != NULL) {
        csTypeBeginScope(checker);
        if (node->as.tryStmt.catchName != NULL) {
          /* Anything can be thrown, so the binding is dynamic. */
          csTypeDeclareVariable(checker, node->as.tryStmt.catchName,
                          node->as.tryStmt.catchNameLength, TYPE_ANY);
        }
        checkNode(checker, node->as.tryStmt.catchBody);
        csTypeEndScope(checker);
      }
      checkNode(checker, node->as.tryStmt.finallyBody);
      result = TYPE_UNDEFINED;
      break;

    case AST_THROW_STMT:
      checkNode(checker, node->as.thrown);
      result = TYPE_UNDEFINED;
      break;

    /* Class types are not modelled. The lattice here is a fixed set of
     * primitives, and adding nominal types with members and subtyping is a
     * milestone of its own — so an instance is `object`, a class is dynamic,
     * and `this` is dynamic. Nothing about a class is checked statically
     * beyond what its method bodies say on their own. */
    case AST_IMPORT:
      if (node->as.import.namespaceName != NULL) {
        csTypeDeclareVariable(checker, node->as.import.namespaceName,
                        node->as.import.namespaceLength, TYPE_OBJECT);
      }
      for (int i = 0; i < node->as.import.nameCount; i++) {
        const AstModuleName *entry = &node->as.import.names[i];
        csTypeDeclareVariable(checker, entry->alias, entry->aliasLength, TYPE_ANY);
      }
      result = TYPE_UNDEFINED;
      break;

    case AST_EXPORT:
      checkNode(checker, node->as.export.declaration);
      result = TYPE_UNDEFINED;
      break;

    case AST_CLASS_DECL: {
      csTypeDeclareVariable(checker, node->as.classDecl.name, node->as.classDecl.nameLength,
                      TYPE_ANY);

      for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
        checkNode(checker, node->as.classDecl.fields[i].initializer);
      }
      if (node->as.classDecl.constructor != NULL) {
        checkNode(checker, node->as.classDecl.constructor);
      }
      for (int i = 0; i < node->as.classDecl.memberCount; i++) {
        checkNode(checker, node->as.classDecl.members[i].function);
      }
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_FOR_OF_STMT:
      csTypeBeginScope(checker);
      checkNode(checker, node->as.forOf.iterable);
      /* Element types are not modelled, so the binding is dynamic. */
      csTypeDeclareVariable(checker, node->as.forOf.name, node->as.forOf.nameLength,
                      TYPE_ANY);
      checkNode(checker, node->as.forOf.body);
      csTypeEndScope(checker);
      result = TYPE_UNDEFINED;
      break;

    case AST_FOR_STMT:
      /* The initialiser is scoped to the loop, matching the compiler. */
      csTypeBeginScope(checker);
      checkNode(checker, node->as.forStmt.initializer);
      checkNode(checker, node->as.forStmt.condition);
      checkNode(checker, node->as.forStmt.increment);
      checkNode(checker, node->as.forStmt.body);
      csTypeEndScope(checker);
      result = TYPE_UNDEFINED;
      break;

    case AST_PROGRAM:
      for (int i = 0; i < node->as.program.count; i++) {
        checkNode(checker, node->as.program.statements[i]);
      }
      result = TYPE_UNDEFINED;
      break;

    default:
      return false;
  }

  *out = result;
  return true;
}
