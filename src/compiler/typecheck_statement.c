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

bool checkStatementNode(Checker *checker, AstNode *node, TypeId *out) {
  TypeId result = TYPE_DYNAMIC;

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
      TypeId subject = checkNode(checker, node->as.switchStmt.subject);
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        TypeId test = checkNode(checker, node->as.switchStmt.cases[i].test);
        /* Arms are matched with ===, so an arm that can never match is the
         * same mistake as writing that comparison out by hand. */
        if (csTypeIsKnown(subject) && csTypeIsKnown(test) && subject != test) {
          csTypeError(checker, node->as.switchStmt.cases[i].test->line, "this case is %s but the switch subject is %s, so it can never match",
                      csTypeNameIn(checker->types, test), csTypeNameIn(checker->types, subject));
        }
        checkNode(checker, node->as.switchStmt.cases[i].body);
      }
      checkNode(checker, node->as.switchStmt.defaultBody);
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_FUNCTION: {
      Signature *signature = csTypeDeclareFunction(checker, node);
      csTypeCheckFunctionBody(checker, node, signature);
      /* A function written where a value is wanted answers its own type, so
       * `(n: number) => "n=" + n` passed to a `(n: number) => string`
       * parameter is checked against it rather than merely being callable. */
      result = signature != NULL ? signature->type : TYPE_FUNCTION;
      break;
    }

    case AST_RETURN_STMT: {
      TypeId returned = node->as.returnValue != NULL ? checkNode(checker, node->as.returnValue) : TYPE_UNDEFINED;
      if (checker->currentReturnAnnotated) returned = csTypeCheckShape(checker, node->as.returnValue, checker->currentReturn);
      if (checker->functionDepth == 0) {
        /* The compiler reports this with better placement. */
        result = TYPE_UNDEFINED;
        break;
      }
      /* An unannotated function learns what it answers from here. Two returns
       * of different types make a union, which is exactly what the function
       * answers. */
      if (!checker->currentReturnAnnotated && checker->functionDepth > 0) {
        checker->inferredReturn = checker->inferredReturn == TYPE_ERROR ? returned : csTypeUnionWith(checker->types, checker->inferredReturn, returned);
      }
      if (checker->currentReturnAnnotated && !csTypeAssignableIn(checker->types, returned, checker->currentReturn)) {
        csTypeError(checker, node->line, "cannot return %s from a function declared %s", csTypeNameIn(checker->types, returned),
                    csTypeNameIn(checker->types, checker->currentReturn));
      }
      result = TYPE_UNDEFINED;
      break;
    }

    case AST_VAR_DECL: {
      TypeId initializer = node->as.varDecl.initializer != NULL ? checkNode(checker, node->as.varDecl.initializer) : TYPE_UNDEFINED;

      TypeId declared;
      bool awaiting = false;
      if (node->as.varDecl.hasAnnotation) {
        declared = node->as.varDecl.declaredType;
        /* `const p: Point = { x: 1, y: 2 }` — the literal is proved against
         * the interface here, where its members are still written down. */
        initializer = csTypeCheckShape(checker, node->as.varDecl.initializer, declared);
        if (!csTypeAssignableIn(checker->types, initializer, declared)) {
          csTypeError(checker, node->line, "cannot assign %s to '%.*s', declared as %s", csTypeNameIn(checker->types, initializer), node->as.varDecl.length,
                      node->as.varDecl.name, csTypeNameIn(checker->types, declared));
        }
      } else if (node->as.varDecl.initializer != NULL && initializer != TYPE_NULL && initializer != TYPE_UNDEFINED) {
        /* Inference: an unannotated declaration takes its initialiser's type,
         * and keeps it. `let total = 0` is a number from here on, and
         * assigning a string to it is an error rather than a surprise. */
        declared = initializer;
      } else {
        /* `let x;`, or one initialised with null or undefined: nothing to
         * learn from yet. The first assignment that carries a type settles it
         * — see Variable.awaiting. */
        declared = TYPE_UNDEFINED;
        awaiting = true;
      }

      if (awaiting) {
        csTypeDeclareAwaiting(checker, node->as.varDecl.name, node->as.varDecl.length);
      } else {
        csTypeDeclareVariable(checker, node->as.varDecl.name, node->as.varDecl.length, declared);
      }
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

    case AST_IF_STMT: {
      checkNode(checker, node->as.ifStmt.condition);

      /* `if (typeof x === "number")` proves something about x inside the
       * branch and nothing outside it, so the narrowed type is put back
       * afterwards. This is what makes a `value` usable at all. */
#define NARROWED_AT_ONCE 8
      Variable *narrowed[NARROWED_AT_ONCE];
      TypeId saved[NARROWED_AT_ONCE];
      int count = csTypeNarrowAll(checker, node->as.ifStmt.condition, true, narrowed, saved, NARROWED_AT_ONCE);
      checkNode(checker, node->as.ifStmt.thenBranch);
      for (int i = 0; i < count; i++) narrowed[i]->type = saved[i];

      /* And `!==` proves it about the other one. */
      int elseCount = csTypeNarrowAll(checker, node->as.ifStmt.condition, false, narrowed, saved, NARROWED_AT_ONCE);
      checkNode(checker, node->as.ifStmt.elseBranch);

      /* A guard keeps its proof. `if (typeof x !== "number") return;` means
       * everything after the `if` runs only when x *is* a number, so the
       * narrowing is not put back — which is what lets a function check its
       * arguments once at the top rather than inside every use. The scope this
       * block belongs to drops the variables in the ordinary way. */
      if (!csTypeBranchAlwaysLeaves(node->as.ifStmt.thenBranch)) {
        for (int i = 0; i < elseCount; i++) narrowed[i]->type = saved[i];
      }
#undef NARROWED_AT_ONCE

      result = TYPE_UNDEFINED;
      break;
    }

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
          csTypeDeclareVariable(checker, node->as.tryStmt.catchName, node->as.tryStmt.catchNameLength, TYPE_DYNAMIC);
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
        csTypeDeclareVariable(checker, node->as.import.namespaceName, node->as.import.namespaceLength, TYPE_OBJECT);
      }
      /* Types cross the boundary with the binding. The module was loaded and
       * checked before this file was — the loader reads dependencies first —
       * so what it exports is already settled, and importing a type is
       * re-interning it into this file's table. */
      for (int i = 0; i < node->as.import.nameCount; i++) {
        const AstModuleName *entry = &node->as.import.names[i];
        TypeId imported = csTypeImportedBinding(checker, node, entry->name, entry->nameLength);
        csTypeDeclareVariable(checker, entry->alias, entry->aliasLength, imported);
      }
      if (node->as.import.defaultName != NULL) {
        TypeId imported = csTypeImportedBinding(checker, node, "default", 7);
        csTypeDeclareVariable(checker, node->as.import.defaultName, node->as.import.defaultLength, imported);
      }
      result = TYPE_UNDEFINED;
      break;

    case AST_EXPORT:
      checkNode(checker, node->as.export.declaration);
      /* What this file gives away, and what each one is: read by whatever
       * imports the file, which is the only way a type crosses a boundary. */
      csTypeRecordExports(checker, node);
      result = TYPE_UNDEFINED;
      break;

    case AST_CLASS_DECL: {
      /* A class expression — `const C = class {}` — binds nothing of its own,
       * and answers the class as a value. Saying `undefined` there would make
       * `new C()` a call on undefined, which is the declaration's answer
       * rather than the expression's. */
      if (!node->as.classDecl.isExpression) {
        csTypeDeclareVariable(checker, node->as.classDecl.name, node->as.classDecl.nameLength, TYPE_DYNAMIC);
      }

      for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
        checkNode(checker, node->as.classDecl.fields[i].initializer);
      }
      if (node->as.classDecl.constructor != NULL) {
        checkNode(checker, node->as.classDecl.constructor);
      }
      for (int i = 0; i < node->as.classDecl.memberCount; i++) {
        checkNode(checker, node->as.classDecl.members[i].function);
      }
      result = node->as.classDecl.isExpression ? TYPE_FUNCTION : TYPE_UNDEFINED;
      break;
    }

    case AST_FOR_OF_STMT:
      csTypeBeginScope(checker);
      checkNode(checker, node->as.forOf.iterable);
      /* Element types are not modelled, so the binding is dynamic. */
      csTypeDeclareVariable(checker, node->as.forOf.name, node->as.forOf.nameLength, TYPE_DYNAMIC);
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

    default: return false;
  }

  *out = result;
  return true;
}
