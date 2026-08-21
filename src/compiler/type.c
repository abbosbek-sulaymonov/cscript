#include <string.h>

#include "cscript/type.h"

const char *csTypeName(TypeKind type) {
  switch (type) {
    case TYPE_ANY:       return "any";
    case TYPE_NUMBER:    return "number";
    case TYPE_BIGINT:    return "bigint";
    case TYPE_STRING:    return "string";
    case TYPE_BOOLEAN:   return "boolean";
    case TYPE_NULL:      return "null";
    case TYPE_UNDEFINED: return "undefined";
    case TYPE_FUNCTION:  return "function";
    case TYPE_OBJECT:    return "object";
    case TYPE_ERROR:     return "<error>";
  }
  return "<unknown>";
}

bool csTypeFromName(const char *name, int length, TypeKind *out) {
  static const struct {
    const char *name;
    TypeKind type;
  } table[] = {
      {"any", TYPE_ANY},           {"number", TYPE_NUMBER},
      {"bigint", TYPE_BIGINT},
      {"string", TYPE_STRING},     {"boolean", TYPE_BOOLEAN},
      {"null", TYPE_NULL},         {"undefined", TYPE_UNDEFINED},
      {"object", TYPE_OBJECT},
  };

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if ((int)strlen(table[i].name) == length &&
        memcmp(table[i].name, name, (size_t)length) == 0) {
      *out = table[i].type;
      return true;
    }
  }
  return false;
}

bool csTypeAssignable(TypeKind from, TypeKind to) {
  /* Anything already in error absorbs further complaints. */
  if (from == TYPE_ERROR || to == TYPE_ERROR) return true;

  /* The gradual boundary: `any` flows both ways without a check. */
  if (from == TYPE_ANY || to == TYPE_ANY) return true;

  return from == to;
}
