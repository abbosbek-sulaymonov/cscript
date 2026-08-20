/* native_json.c — JSON.stringify and JSON.parse.
 *
 * stringify is a straight recursive walk; parse is a recursive-descent parser
 * over the source text. Neither shares code with the language's own printer or
 * lexer, because JSON is deliberately not JavaScript: it has no trailing
 * commas, no single quotes, no unquoted keys, and no undefined.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/memory.h"
#include "cscript/native.h"
#include "cscript/object.h"
#include "cscript/vm.h"

/* ---------------- stringify ---------------- */

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  bool failed;
} JsonBuffer;

static void jsonAppend(JsonBuffer *out, const char *text, size_t length) {
  if (out->failed) return;
  if (out->length + length + 1 > out->capacity) {
    size_t capacity = out->capacity < 64 ? 64 : out->capacity;
    while (capacity < out->length + length + 1) capacity *= 2;
    char *grown = (char *)realloc(out->data, capacity);
    if (grown == NULL) {
      out->failed = true;
      return;
    }
    out->data = grown;
    out->capacity = capacity;
  }
  memcpy(out->data + out->length, text, length);
  out->length += length;
  out->data[out->length] = '\0';
}

static void jsonAppendString(JsonBuffer *out, const char *chars, int length) {
  jsonAppend(out, "\"", 1);
  for (int i = 0; i < length; i++) {
    char c = chars[i];
    switch (c) {
      case '"':  jsonAppend(out, "\\\"", 2); break;
      case '\\': jsonAppend(out, "\\\\", 2); break;
      case '\n': jsonAppend(out, "\\n", 2); break;
      case '\t': jsonAppend(out, "\\t", 2); break;
      case '\r': jsonAppend(out, "\\r", 2); break;
      default:
        if ((unsigned char)c < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04x", (unsigned char)c);
          jsonAppend(out, escape, 6);
        } else {
          jsonAppend(out, &c, 1);
        }
        break;
    }
  }
  jsonAppend(out, "\"", 1);
}

/* Returns false when the value has no JSON form at all, which only happens for
 * a bare undefined or function at the top level. */
static bool jsonWrite(JsonBuffer *out, Value value) {
  if (IS_NULL(value)) {
    jsonAppend(out, "null", 4);
    return true;
  }
  if (IS_BOOL(value)) {
    if (AS_BOOL(value)) {
      jsonAppend(out, "true", 4);
    } else {
      jsonAppend(out, "false", 5);
    }
    return true;
  }
  if (IS_NUMBER(value)) {
    double number = AS_NUMBER(value);
    /* JSON has no way to write these, so JavaScript emits null. */
    if (isnan(number) || isinf(number)) {
      jsonAppend(out, "null", 4);
      return true;
    }
    size_t length = 0;
    char *text = csValueToCString(value, &length);
    if (text == NULL) {
      out->failed = true;
      return true;
    }
    jsonAppend(out, text, length);
    free(text);
    return true;
  }
  if (IS_STRING(value)) {
    jsonAppendString(out, AS_CSTRING(value), AS_STRING(value)->length);
    return true;
  }
  /* A Date writes itself as the string `toJSON` would give, which is what
   * makes a timestamp survive a round trip through JSON at all. An invalid one
   * has no such string and becomes null, as it does in JavaScript. */
  if (IS_DATE(value)) {
    char text[64];
    if (!csDateToISO(AS_DATE(value)->ms, text, sizeof text)) return false;
    jsonAppendString(out, text, (int)strlen(text));
    return true;
  }
  if (IS_ARRAY(value)) {
    ObjArray *array = AS_ARRAY(value);
    jsonAppend(out, "[", 1);
    for (int i = 0; i < array->elements.count; i++) {
      if (i > 0) jsonAppend(out, ",", 1);
      /* An element with no JSON form becomes null rather than vanishing. */
      if (!jsonWrite(out, array->elements.values[i])) jsonAppend(out, "null", 4);
    }
    jsonAppend(out, "]", 1);
    return true;
  }
  if (IS_OBJECT(value)) {
    ObjObject *object = AS_OBJECT(value);
    jsonAppend(out, "{", 1);
    bool first = true;
    for (int i = 0; i < csObjectCount(object); i++) {
      ObjString *key = csObjectKeyAt(object, i);
      Value property = csObjectValueAt(object, i);

      /* A property with no JSON form is omitted entirely. */
      JsonBuffer probe = {NULL, 0, 0, false};
      bool writable = jsonWrite(&probe, property);
      free(probe.data);
      if (!writable) continue;

      if (!first) jsonAppend(out, ",", 1);
      first = false;
      jsonAppendString(out, key->chars, key->length);
      jsonAppend(out, ":", 1);
      jsonWrite(out, property);
    }
    jsonAppend(out, "}", 1);
    return true;
  }

  /* undefined and functions have no JSON form. */
  return false;
}

static bool jsonStringify(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1) {
    csVMRuntimeError("JSON.stringify expects a value");
    return false;
  }

  JsonBuffer out = {NULL, 0, 0, false};
  bool writable = jsonWrite(&out, args[0]);
  if (out.failed) {
    free(out.data);
    csVMRuntimeError("out of memory in JSON.stringify");
    return false;
  }
  if (!writable) {
    free(out.data);
    *result = UNDEFINED_VAL; /* JSON.stringify(undefined) is undefined */
    return true;
  }

  *result = OBJ_VAL(csStringCopy(out.data != NULL ? out.data : "", (int)out.length));
  free(out.data);
  return true;
}

/* ---------------- parse ---------------- */

typedef struct {
  const char *cursor;
  const char *end;
  bool failed;
} JsonParser;

static void skipWhitespace(JsonParser *parser) {
  while (parser->cursor < parser->end) {
    char c = *parser->cursor;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    parser->cursor++;
  }
}

static bool parseValue(JsonParser *parser, Value *out);

static bool parseLiteral(JsonParser *parser, const char *text, Value value,
                         Value *out) {
  size_t length = strlen(text);
  if ((size_t)(parser->end - parser->cursor) < length ||
      memcmp(parser->cursor, text, length) != 0) {
    return false;
  }
  parser->cursor += length;
  *out = value;
  return true;
}

static bool parseString(JsonParser *parser, Value *out) {
  parser->cursor++; /* opening quote */

  size_t capacity = 32;
  size_t length = 0;
  char *buffer = (char *)malloc(capacity);
  if (buffer == NULL) {
    parser->failed = true;
    return false;
  }

  while (parser->cursor < parser->end && *parser->cursor != '"') {
    char c = *parser->cursor++;
    if (c == '\\' && parser->cursor < parser->end) {
      char escaped = *parser->cursor++;
      switch (escaped) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'u': {
          /* Only the Basic Latin range is decoded; anything else is passed
           * through as a literal '?', matching the ASCII-only string model. */
          if (parser->end - parser->cursor < 4) break;
          char digits[5] = {parser->cursor[0], parser->cursor[1], parser->cursor[2],
                            parser->cursor[3], '\0'};
          parser->cursor += 4;
          long code = strtol(digits, NULL, 16);
          c = code < 128 ? (char)code : '?';
          break;
        }
        default: c = escaped; break;
      }
    }

    if (length + 2 > capacity) {
      capacity *= 2;
      char *grown = (char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(buffer);
        parser->failed = true;
        return false;
      }
      buffer = grown;
    }
    buffer[length++] = c;
  }

  if (parser->cursor >= parser->end) {
    free(buffer);
    return false; /* unterminated */
  }
  parser->cursor++; /* closing quote */

  buffer[length] = '\0';
  *out = OBJ_VAL(csStringCopy(buffer, (int)length));
  free(buffer);
  return true;
}

static bool parseArray(JsonParser *parser, Value *out) {
  parser->cursor++; /* '[' */

  ObjArray *array = csArrayNew();
  csPushTempRoot((Obj *)array);

  skipWhitespace(parser);
  if (parser->cursor < parser->end && *parser->cursor == ']') {
    parser->cursor++;
    csPopTempRoot();
    *out = OBJ_VAL(array);
    return true;
  }

  for (;;) {
    Value element;
    if (!parseValue(parser, &element)) {
      csPopTempRoot();
      return false;
    }
    /* The element may be a fresh object the collector cannot yet reach. */
    if (IS_OBJ(element)) csPushTempRoot(AS_OBJ(element));
    csValueArrayWrite(&array->elements, element);
    if (IS_OBJ(element)) csPopTempRoot();

    skipWhitespace(parser);
    if (parser->cursor >= parser->end) break;
    if (*parser->cursor == ',') {
      parser->cursor++;
      continue;
    }
    if (*parser->cursor == ']') {
      parser->cursor++;
      csPopTempRoot();
      *out = OBJ_VAL(array);
      return true;
    }
    break;
  }

  csPopTempRoot();
  return false;
}

static bool parseObject(JsonParser *parser, Value *out) {
  parser->cursor++; /* '{' */

  ObjObject *object = csObjectNew("Object");
  csPushTempRoot((Obj *)object);

  skipWhitespace(parser);
  if (parser->cursor < parser->end && *parser->cursor == '}') {
    parser->cursor++;
    csPopTempRoot();
    *out = OBJ_VAL(object);
    return true;
  }

  for (;;) {
    skipWhitespace(parser);
    if (parser->cursor >= parser->end || *parser->cursor != '"') break;

    Value key;
    if (!parseString(parser, &key)) break;
    csPushTempRoot(AS_OBJ(key));

    skipWhitespace(parser);
    if (parser->cursor >= parser->end || *parser->cursor != ':') {
      csPopTempRoot();
      break;
    }
    parser->cursor++;

    Value property;
    if (!parseValue(parser, &property)) {
      csPopTempRoot();
      break;
    }
    if (IS_OBJ(property)) csPushTempRoot(AS_OBJ(property));
    csObjectPut(object, AS_STRING(key), property);
    if (IS_OBJ(property)) csPopTempRoot();
    csPopTempRoot(); /* key */

    skipWhitespace(parser);
    if (parser->cursor >= parser->end) break;
    if (*parser->cursor == ',') {
      parser->cursor++;
      continue;
    }
    if (*parser->cursor == '}') {
      parser->cursor++;
      csPopTempRoot();
      *out = OBJ_VAL(object);
      return true;
    }
    break;
  }

  csPopTempRoot();
  return false;
}

static bool parseValue(JsonParser *parser, Value *out) {
  skipWhitespace(parser);
  if (parser->cursor >= parser->end) return false;

  char c = *parser->cursor;
  if (c == '{') return parseObject(parser, out);
  if (c == '[') return parseArray(parser, out);
  if (c == '"') return parseString(parser, out);
  if (c == 't') return parseLiteral(parser, "true", BOOL_VAL(true), out);
  if (c == 'f') return parseLiteral(parser, "false", BOOL_VAL(false), out);
  if (c == 'n') return parseLiteral(parser, "null", NULL_VAL, out);

  char *end = NULL;
  double number = strtod(parser->cursor, &end);
  if (end == parser->cursor) return false;
  parser->cursor = end;
  *out = NUMBER_VAL(number);
  return true;
}

static bool jsonParse(Value receiver, int argCount, Value *args, Value *result) {
  (void)receiver;
  if (argCount < 1 || !IS_STRING(args[0])) {
    csVMRuntimeError("JSON.parse expects a string, got %s",
                     argCount >= 1 ? csValueTypeName(args[0]) : "no argument");
    return false;
  }

  ObjString *source = AS_STRING(args[0]);
  JsonParser parser = {source->chars, source->chars + source->length, false};

  Value parsed;
  if (!parseValue(&parser, &parsed) || parser.failed) {
    csVMRuntimeError("invalid JSON");
    return false;
  }

  skipWhitespace(&parser);
  if (parser.cursor != parser.end) {
    csVMRuntimeError("invalid JSON: unexpected trailing text");
    return false;
  }

  *result = parsed;
  return true;
}

void csJsonInstall(ObjObject *json);

void csJsonInstall(ObjObject *json) {
  ObjNative *stringify = csNativeNew(jsonStringify, "stringify", -1);
  csPushTempRoot((Obj *)stringify);
  csObjectSetProperty(json, "stringify", OBJ_VAL(stringify));
  csPopTempRoot();

  ObjNative *parse = csNativeNew(jsonParse, "parse", 1);
  csPushTempRoot((Obj *)parse);
  csObjectSetProperty(json, "parse", OBJ_VAL(parse));
  csPopTempRoot();
}
