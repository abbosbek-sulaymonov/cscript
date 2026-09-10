/* ast_name.c — an operator's name, for the diagnostics and the AST dump.
 *
 * The one thing in the AST that is not construction. Kept apart because it is
 * the only place the operator enums are spelled out in prose, and a new
 * operator that reaches the parser without reaching here prints as a
 * question mark rather than failing to build.
 */
#include <stdlib.h>
#include <string.h>

#include "cscript/ast.h"


const char *csUnaryOpName(UnaryOp op) {
  switch (op) {
    case UNARY_NEGATE: return "-";
    case UNARY_NOT:    return "!";
    case UNARY_TYPEOF: return "typeof";
    case UNARY_VOID:   return "void";
  }
  return "?";
}

const char *csBinaryOpName(BinaryOp op) {
  switch (op) {
    case BINARY_ADD:              return "+";
    case BINARY_SUBTRACT:         return "-";
    case BINARY_MULTIPLY:         return "*";
    case BINARY_DIVIDE:           return "/";
    case BINARY_MODULO:           return "%";
    case BINARY_EXPONENT:         return "**";
    case BINARY_EQUAL:            return "===";
    case BINARY_NOT_EQUAL:        return "!==";
    case BINARY_GREATER:          return ">";
    case BINARY_GREATER_EQUAL:    return ">=";
    case BINARY_LESS:             return "<";
    case BINARY_LESS_EQUAL:       return "<=";
    case BINARY_INSTANCEOF:       return "instanceof";
    case BINARY_IN:               return "in";
  }
  return "?";
}

const char *csLogicalOpName(LogicalOp op) {
  switch (op) {
    case LOGICAL_AND:     return "&&";
    case LOGICAL_OR:      return "||";
    case LOGICAL_NULLISH: return "?\?";
  }
  return "?";
}
