/* debug_ast.c — the parse tree, printed.
 *
 * One line per node, indented by depth, with the type the checker resolved
 * beside it — which is the whole reason to look at this rather than at the
 * bytecode: it shows what the checker concluded, not what the compiler did
 * about it.
 */
#include <stdarg.h>
#include <stdio.h>

#include "cscript/debug.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/value.h"

static void indent(int depth) {
  for (int i = 0; i < depth; i++) printf("  ");
}

/* Annotates a dumped node with the type the checker resolved, so `make trace`
 * shows what the compiler actually knows. */
static void printType(const AstNode *node) {
  if (node->resolvedType != TYPE_ANY) printf("  : %s", csTypeName(node->resolvedType));
}

static void printNode(const AstNode *node, int depth) {
  if (node == NULL) {
    indent(depth);
    printf("<null>\n");
    return;
  }

  indent(depth);
  switch (node->type) {
    case AST_NUMBER_LITERAL:
      printf("Number ");
      csValuePrint(NUMBER_VAL(node->as.number));
      printType(node);
      printf("\n");
      break;
    case AST_STRING_LITERAL:
      printf("String \"%.*s\"\n", node->as.string.length, node->as.string.chars);
      break;
    case AST_BIGINT_LITERAL:
      printf("BigInt %.*sn\n", node->as.string.length, node->as.string.chars);
      break;
    case AST_BOOL_LITERAL:
      printf("Bool %s\n", node->as.boolean ? "true" : "false");
      break;
    case AST_NULL_LITERAL:
      printf("Null\n");
      break;
    case AST_UNDEFINED_LITERAL:
      printf("Undefined\n");
      break;
    case AST_UNARY:
      printf("Unary %s\n", csUnaryOpName(node->as.unary.op));
      printNode(node->as.unary.operand, depth + 1);
      break;
    case AST_BINARY:
      printf("Binary %s", csBinaryOpName(node->as.binary.op));
      printType(node);
      printf("\n");
      printNode(node->as.binary.left, depth + 1);
      printNode(node->as.binary.right, depth + 1);
      break;
    case AST_LOGICAL:
      printf("Logical %s\n", csLogicalOpName(node->as.logical.op));
      printNode(node->as.logical.left, depth + 1);
      printNode(node->as.logical.right, depth + 1);
      break;
    case AST_GROUPING:
      printf("Grouping\n");
      printNode(node->as.grouping, depth + 1);
      break;
    case AST_OPTIONAL_CHAIN:
      printf("OptionalChain\n");
      printNode(node->as.expression, depth + 1);
      break;
    case AST_LABELED_STMT:
      printf("Label %.*s\n", node->as.labeled.length, node->as.labeled.name);
      printNode(node->as.labeled.body, depth + 1);
      break;
    case AST_TEMPLATE_STRINGS:
      printf("TemplateStrings\n");
      printNode(node->as.templateStrings.cooked, depth + 1);
      break;
    case AST_SEQUENCE:
      printf("Sequence\n");
      printNode(node->as.sequence.first, depth + 1);
      printNode(node->as.sequence.second, depth + 1);
      break;
    case AST_YIELD:
      printf(node->as.yield.isDelegate ? "YieldFrom\n" : "Yield\n");
      printNode(node->as.yield.value, depth + 1);
      break;
    case AST_DELETE:
      printf("Delete\n");
      printNode(node->as.deleteTarget, depth + 1);
      break;
    case AST_IDENTIFIER:
      printf("Identifier %.*s", node->as.identifier.length, node->as.identifier.name);
      printType(node);
      printf("\n");
      break;
    case AST_ASSIGN:
      printf("Assign\n");
      printNode(node->as.assign.target, depth + 1);
      printNode(node->as.assign.value, depth + 1);
      break;
    case AST_UPDATE:
      printf("Update %s%s\n", node->as.update.isIncrement ? "++" : "--",
             node->as.update.isPrefix ? " (prefix)" : " (postfix)");
      printNode(node->as.update.target, depth + 1);
      break;
    case AST_FUNCTION:
      printf("Function %.*s (%d param%s)\n",
             node->as.function.name != NULL ? node->as.function.nameLength : 11,
             node->as.function.name != NULL ? node->as.function.name : "<anonymous>",
             node->as.function.paramCount,
             node->as.function.paramCount == 1 ? "" : "s");
      printNode(node->as.function.body, depth + 1);
      break;
    case AST_RETURN_STMT:
      printf("Return\n");
      printNode(node->as.returnValue, depth + 1);
      break;
    case AST_CONDITIONAL:
      printf("Conditional\n");
      printNode(node->as.conditional.condition, depth + 1);
      printNode(node->as.conditional.thenValue, depth + 1);
      printNode(node->as.conditional.elseValue, depth + 1);
      break;
    case AST_BREAK_STMT:
      printf("Break\n");
      break;
    case AST_CONTINUE_STMT:
      printf("Continue\n");
      break;
    case AST_SWITCH_STMT:
      printf("Switch (%d case%s)\n", node->as.switchStmt.caseCount,
             node->as.switchStmt.caseCount == 1 ? "" : "s");
      printNode(node->as.switchStmt.subject, depth + 1);
      for (int i = 0; i < node->as.switchStmt.caseCount; i++) {
        printNode(node->as.switchStmt.cases[i].test, depth + 1);
        printNode(node->as.switchStmt.cases[i].body, depth + 2);
      }
      if (node->as.switchStmt.defaultBody != NULL) {
        printNode(node->as.switchStmt.defaultBody, depth + 1);
      }
      break;
    case AST_TRY_STMT:
      printf("Try\n");
      printNode(node->as.tryStmt.body, depth + 1);
      if (node->as.tryStmt.catchBody != NULL) {
        printNode(node->as.tryStmt.catchBody, depth + 1);
      }
      if (node->as.tryStmt.finallyBody != NULL) {
        printNode(node->as.tryStmt.finallyBody, depth + 1);
      }
      break;
    case AST_THROW_STMT:
      printf("Throw\n");
      printNode(node->as.thrown, depth + 1);
      break;
    case AST_IMPORT:
      printf("Import \"%.*s\"\n", node->as.import.specifierLength,
             node->as.import.specifier);
      break;
    case AST_EXPORT:
      printf("Export\n");
      printNode(node->as.export.declaration, depth + 1);
      break;
    case AST_AWAIT:
      printf("Await\n");
      printNode(node->as.unary.operand, depth + 1);
      break;
    case AST_REGEX_LITERAL:
      printf("Regex /%.*s/%.*s\n", node->as.regex.sourceLength, node->as.regex.source,
             node->as.regex.flagsLength, node->as.regex.flags);
      break;
    case AST_DYNAMIC_IMPORT:
      printf("DynamicImport\n");
      printNode(node->as.unary.operand, depth + 1);
      break;
    case AST_NEW_TARGET:
      printf("NewTarget\n");
      break;
    case AST_THIS:
      printf("This\n");
      break;
    case AST_SUPER:
      if (node->as.super.name != NULL) {
        printf("Super .%.*s\n", node->as.super.length, node->as.super.name);
      } else {
        printf("Super ()\n");
      }
      break;
    case AST_CLASS_DECL:
      printf("Class %.*s", node->as.classDecl.nameLength, node->as.classDecl.name);
      if (node->as.classDecl.superName != NULL) {
        printf(" extends %.*s", node->as.classDecl.superLength,
               node->as.classDecl.superName);
      }
      printf("\n");
      for (int i = 0; i < node->as.classDecl.fieldCount; i++) {
        indent(depth + 1);
        printf("Field %.*s\n", node->as.classDecl.fields[i].length,
               node->as.classDecl.fields[i].name);
        printNode(node->as.classDecl.fields[i].initializer, depth + 2);
      }
      printNode(node->as.classDecl.constructor, depth + 1);
      for (int i = 0; i < node->as.classDecl.memberCount; i++) {
        printNode(node->as.classDecl.members[i].function, depth + 1);
      }
      break;
    case AST_SPREAD:
      printf("Spread\n");
      printNode(node->as.spread, depth + 1);
      break;
    case AST_DESTRUCTURE:
      printf("Destructure %s (%d binding%s)\n",
             node->as.destructure.isObject ? "object" : "array",
             node->as.destructure.count,
             node->as.destructure.count == 1 ? "" : "s");
      printNode(node->as.destructure.initializer, depth + 1);
      break;
    case AST_INDEX:
      printf("Index\n");
      printNode(node->as.index.target, depth + 1);
      printNode(node->as.index.index, depth + 1);
      break;
    case AST_OBJECT_LITERAL:
      printf("ObjectLiteral (%d)\n", node->as.objectLiteral.count);
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        printNode(node->as.objectLiteral.keys[i], depth + 1);
        printNode(node->as.objectLiteral.values[i], depth + 2);
      }
      break;
    case AST_ARRAY_LITERAL:
      printf("ArrayLiteral (%d)\n", node->as.arrayLiteral.count);
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        printNode(node->as.arrayLiteral.elements[i], depth + 1);
      }
      break;
    case AST_PROPERTY:
      printf("Property .%.*s\n", node->as.property.length, node->as.property.name);
      printNode(node->as.property.object, depth + 1);
      break;
    case AST_CALL:
      printf("Call (%d argument%s)\n", node->as.call.argCount,
             node->as.call.argCount == 1 ? "" : "s");
      printNode(node->as.call.callee, depth + 1);
      for (int i = 0; i < node->as.call.argCount; i++) {
        printNode(node->as.call.arguments[i], depth + 1);
      }
      break;
    case AST_EXPRESSION_STMT:
      printf("ExpressionStmt\n");
      printNode(node->as.expression, depth + 1);
      break;
    case AST_VAR_DECL:
      printf("%s %.*s\n", node->as.varDecl.isConst ? "Const" : "Let",
             node->as.varDecl.length, node->as.varDecl.name);
      if (node->as.varDecl.initializer != NULL) {
        printNode(node->as.varDecl.initializer, depth + 1);
      }
      break;
    case AST_BLOCK:
      printf("Block (%d statement%s)\n", node->as.block.count,
             node->as.block.count == 1 ? "" : "s");
      for (int i = 0; i < node->as.block.count; i++) {
        printNode(node->as.block.statements[i], depth + 1);
      }
      break;
    case AST_IF_STMT:
      printf("If\n");
      printNode(node->as.ifStmt.condition, depth + 1);
      printNode(node->as.ifStmt.thenBranch, depth + 1);
      if (node->as.ifStmt.elseBranch != NULL) {
        printNode(node->as.ifStmt.elseBranch, depth + 1);
      }
      break;
    case AST_WHILE_STMT:
      printf("While\n");
      printNode(node->as.whileStmt.condition, depth + 1);
      printNode(node->as.whileStmt.body, depth + 1);
      break;
    case AST_FOR_OF_STMT:
      printf("ForOf %.*s\n", node->as.forOf.nameLength, node->as.forOf.name);
      printNode(node->as.forOf.iterable, depth + 1);
      printNode(node->as.forOf.body, depth + 1);
      break;
    case AST_FOR_STMT:
      printf("For\n");
      printNode(node->as.forStmt.initializer, depth + 1);
      printNode(node->as.forStmt.condition, depth + 1);
      printNode(node->as.forStmt.increment, depth + 1);
      printNode(node->as.forStmt.body, depth + 1);
      break;
    case AST_PROGRAM:
      printf("Program (%d statement%s)\n", node->as.program.count,
             node->as.program.count == 1 ? "" : "s");
      for (int i = 0; i < node->as.program.count; i++) {
        printNode(node->as.program.statements[i], depth + 1);
      }
      break;
  }
}

void csAstPrint(const AstNode *node) {
  printf("== ast ==\n");
  printNode(node, 0);
  printf("\n");
}
