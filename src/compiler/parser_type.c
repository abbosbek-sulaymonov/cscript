/* parser_type.c — the type grammar: what may be written after a `:`, and the
 * two declarations that describe a shape rather than produce a value.
 *
 * A type expression, loosest first:
 *
 *     union     ::= suffix ( "|" suffix )*
 *     suffix    ::= primary "[]"*
 *     primary   ::= NAME ( "<" union ( "," union )* ">" )?
 *                 | "(" params ")" "=>" union
 *                 | "(" union ")"
 *                 | "null" | "undefined"
 *
 * Nothing here exists at run time. An interface is checked and erased, which
 * is also what `node --experimental-strip-types` does with the same text, and
 * a type expression is erased with the annotation that holds it.
 *
 * `interface` and `type` are both **contextual**. `type` is an ordinary
 * identifier in every other position and programs already use it as one
 * (`flag.type` in std:cli), so it is a declaration only where a declaration
 * can start and a name and an `=` follow it.
 *
 * A type must be declared before it is named. The parser resolves an
 * annotation the moment it reads it — there is no second pass — so
 * `interface A { b: B }` before `B` exists is an error rather than a forward
 * reference. An interface may name *itself*, because its name is registered
 * before its members are read.
 */
#include <stdlib.h>
#include <string.h>

#include "compiler/parser_internal.h"
#include "compiler/type_internal.h"
#include "compiler/type_internal.h"

static bool parseTypeUnion(Parser *parser, TypeId *out);

/* `interface Name {` — the shape of the declaration, told apart from an
 * expression that merely begins with the word. */
bool startsInterfaceDeclaration(Parser *parser) {
  /* No table means the arena could not allocate one; without it there is
   * nowhere to record a type, so the word is not a declaration. */
  if (parser->types == NULL) return false;
  if (!checkWord(parser, "interface")) return false;
  Lexer probe = parser->lexer;
  return csLexerNext(&probe).type == TOKEN_IDENTIFIER;
}

/* `type Name =` or `type Name<T> =`. Two tokens of lookahead, because `type`
 * alone is a name. */
bool startsTypeAlias(Parser *parser) {
  if (parser->types == NULL) return false;
  if (!checkWord(parser, "type")) return false;
  Lexer probe = parser->lexer;
  if (csLexerNext(&probe).type != TOKEN_IDENTIFIER) return false;
  TokenType after = csLexerNext(&probe).type;
  return after == TOKEN_EQUAL || after == TOKEN_LESS;
}

/* `new Box<number>(…)` — an identifier naming a generic type, followed by `<`.
 *
 * Both halves are needed. A `<` alone could be a comparison, and a name that
 * takes no type arguments could not have written any — so requiring the name
 * to be a generic is what keeps `new Point < limit` a comparison. */
bool startsGenericConstruction(Parser *parser) {
  if (parser->types == NULL || !check(parser, TOKEN_IDENTIFIER)) return false;
  Lexer probe = parser->lexer;
  if (csLexerNext(&probe).type != TOKEN_LESS) return false;

  TypeId named;
  if (!csTypeLookupName(parser->types, parser->current.start, parser->current.length, &named)) return false;
  return csTypeTypeParamCount(parser->types, named) > 0;
}

/* `<T>` and `<T, U>` on a declaration. Each name becomes a type variable that
 * is in scope until the declaration ends — see closeTypeParams. */
bool parseTypeParams(Parser *parser, TypeId *params, int *countOut) {
  *countOut = 0;
  if (!matchToken(parser, TOKEN_LESS)) return true;

  do {
    consume(parser, TOKEN_IDENTIFIER, "expected the name of a type parameter");
    if (parser->diag->panicMode) return false;
    if (*countOut >= CS_MAX_TYPE_PARAMS) {
      errorAtCurrent(parser, "a declaration may take at most four type parameters");
      return false;
    }
    int length;
    const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &length);
    if (name == NULL) return false;

    TypeId declared = csTypeDeclareTypeVar(parser->types, name, length);
    if (declared == TYPE_ERROR) {
      errorAtCurrent(parser, "this file declares more types than the checker can hold");
      return false;
    }
    params[(*countOut)++] = declared;
  } while (matchToken(parser, TOKEN_COMMA));

  consume(parser, TOKEN_GREATER, "expected '>' after the type parameters");
  return !parser->diag->panicMode;
}

/* Type parameters go out of scope with the declaration that introduced them,
 * so that a later `T` means whatever it means there. */
void closeTypeParams(Parser *parser, const TypeId *params, int count) {
  for (int i = 0; i < count; i++) csTypeCloseTypeVar(parser->types, params[i]);
}

/* True when a `(` begins a function type rather than a parenthesised one.
 * `()` and `(name:` can only be parameters; anything else is a type in
 * brackets. */
static bool startsFunctionType(Parser *parser) {
  Lexer probe = parser->lexer;
  Token first = csLexerNext(&probe);
  if (first.type == TOKEN_RIGHT_PAREN || first.type == TOKEN_ELLIPSIS) return true;
  if (first.type != TOKEN_IDENTIFIER) return false;
  TokenType second = csLexerNext(&probe).type;
  return second == TOKEN_COLON || second == TOKEN_QUESTION;
}

/* The parameter list of a function type or of an interface's method. The names
 * are read and dropped: what a function type says is what it takes and what it
 * answers, and TypeScript's grammar requires the names to be there. */
bool parseTypeParameterList(Parser *parser, TypeId *params, int *paramCount, int *requiredCount, bool *hasRest, const char *what) {
  *paramCount = 0;
  *requiredCount = 0;
  *hasRest = false;
  bool sawOptional = false;

  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      if (*paramCount >= CS_MAX_TYPE_PARAMS * 8) {
        errorAtCurrent(parser, "this declares more parameters than the checker can hold");
        return false;
      }
      bool isRest = matchToken(parser, TOKEN_ELLIPSIS);
      consume(parser, TOKEN_IDENTIFIER, "expected the name of a parameter");
      if (parser->diag->panicMode) return false;

      bool optional = matchToken(parser, TOKEN_QUESTION);
      consume(parser, TOKEN_COLON, "expected ':' and a type after a parameter's name");
      if (parser->diag->panicMode) return false;

      if (!parseTypeUnion(parser, &params[(*paramCount)++])) return false;
      if (isRest) {
        *hasRest = true;
        break;
      }
      if (optional) sawOptional = true;
      if (!optional && !sawOptional) *requiredCount = *paramCount;
    } while (matchToken(parser, TOKEN_COMMA));
  }

  consume(parser, TOKEN_RIGHT_PAREN, what);
  return !parser->diag->panicMode;
}

/* `(a: number, b?: string) => boolean` */
static bool parseFunctionType(Parser *parser, TypeId *out) {
  advanceToken(parser); /* `(` */

  TypeId params[CS_MAX_TYPE_PARAMS * 8];
  int paramCount;
  int requiredCount;
  bool hasRest;
  if (!parseTypeParameterList(parser, params, &paramCount, &requiredCount, &hasRest, "expected ')' after the parameters of a function type")) return false;

  consume(parser, TOKEN_ARROW, "expected '=>' and a result type");
  if (parser->diag->panicMode) return false;

  TypeId result;
  if (!parseTypeUnion(parser, &result)) return false;
  *out = csTypeFunctionOf(parser->types, params, paramCount, requiredCount, hasRest, result);
  return true;
}

static bool parseTypePrimary(Parser *parser, TypeId *out) {
  /* `keyof T` — the names of T's members, as a union of them. Contextual, so
   * `keyof` is still an ordinary name everywhere else. */
  if (checkWord(parser, "keyof")) {
    advanceToken(parser);
    TypeId subject;
    if (!parseTypePrimary(parser, &subject)) return false;
    *out = csTypeKeyOf(parser->types, subject);
    return true;
  }

  if (check(parser, TOKEN_LEFT_PAREN)) {
    if (startsFunctionType(parser)) return parseFunctionType(parser, out);
    advanceToken(parser);
    if (!parseTypeUnion(parser, out)) return false;
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the type");
    return !parser->diag->panicMode;
  }

  /* `"admin"` — one string and nothing else, which is what makes a set of
   * names a type. Written as it is written in TypeScript, and in a program. */
  if (check(parser, TOKEN_STRING)) {
    advanceToken(parser);
    AstNode *text = makeStringLiteral(parser, parser->previous.start, parser->previous.length, parser->previous.line);
    if (text == NULL) return false;
    *out = csTypeLiteral(parser->types, text->as.string.chars, text->as.string.length);
    return true;
  }

  /* `[number, string]` — a fixed number of elements, each with its own type.
   * Told apart from `T[]` by position: a `[` that *opens* a type is a tuple,
   * where one that follows a type is the array suffix. */
  if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
    TypeId elements[CS_MAX_TYPE_PARAMS * 4];
    int count = 0;
    if (!check(parser, TOKEN_RIGHT_BRACKET)) {
      do {
        if (count >= (int)(sizeof elements / sizeof elements[0])) {
          errorAtCurrent(parser, "a tuple may hold at most sixteen elements");
          return false;
        }
        if (!parseTypeUnion(parser, &elements[count++])) return false;
      } while (matchToken(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the tuple's elements");
    if (parser->diag->panicMode) return false;
    *out = csTypeTupleOf(parser->types, elements, count);
    return true;
  }

  /* `infer R` — a name for whatever stands here in the type being matched.
   * Only meaningful inside the pattern of a conditional, and it is the parse
   * of that pattern which collects it; anywhere else it is a variable nothing
   * will ever bind, which is what the checker could not work out. */
  if (checkWord(parser, "infer")) {
    Lexer probe = parser->lexer;
    if (csLexerNext(&probe).type == TOKEN_IDENTIFIER) {
      advanceToken(parser); /* `infer` */
      advanceToken(parser); /* the name */

      int length;
      const char *named = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &length);
      if (named == NULL) return false;

      TypeId declared = csTypeDeclareTypeVar(parser->types, named, length);
      if (declared == TYPE_ERROR) {
        errorAtCurrent(parser, "this file declares more types than the checker can hold");
        return false;
      }
      if (parser->inferCount >= CS_MAX_TYPE_PARAMS) {
        errorAtCurrent(parser, "a conditional type may infer at most four names");
        return false;
      }
      parser->inferVars[parser->inferCount++] = declared;
      *out = declared;
      return true;
    }
  }

  /* `42`, and `-1` — one number and nothing else. A leading minus belongs to
   * the literal here rather than being an operator, because a type position
   * has no arithmetic in it for one to be part of. */
  if (check(parser, TOKEN_NUMBER) || (check(parser, TOKEN_MINUS) && !parser->diag->panicMode)) {
    bool negated = matchToken(parser, TOKEN_MINUS);
    if (!matchToken(parser, TOKEN_NUMBER)) {
      errorAtCurrent(parser, "expected a number after '-' in a type");
      return false;
    }
    double value = strtod(parser->previous.start, NULL);
    *out = csTypeNumberLiteral(parser->types, negated ? -value : value);
    return true;
  }

  /* `null` and `undefined` are keywords, so they do not arrive as identifiers
   * even though they are perfectly good type names. */
  if (!matchToken(parser, TOKEN_NULL) && !matchToken(parser, TOKEN_UNDEFINED)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a type name");
  }
  if (parser->diag->panicMode) return false;

  const char *name = parser->previous.start;
  int length = parser->previous.length;
  int line = parser->previous.line;
  /* The utility types are recognised by name rather than declared, because
   * each is a mapping over an argument that has to be known before there is
   * anything to map. Asked before the lookup, so that `Partial` is a type the
   * parser knows even though nothing declared it. */
  int utilityArity = 0;
  csTypeUtility(parser->types, name, length, NULL, -1, &utilityArity);

  if (utilityArity == 0 && !csTypeLookupName(parser->types, name, length, out)) {
    /* Some names are wrong in a way worth answering rather than merely
     * rejecting — `any` above all, which is the one a reader reaches for first
     * and the one this language is built on not having. */
    const char *why = csTypeRejectedName(name, length);
    if (why != NULL) {
      csDiagnosticError(parser->diag, line, name, length, "%s", why);
    } else {
      csDiagnosticError(parser->diag, line, name, length, "unknown type '%.*s'", length, name);
    }
    return false;
  }

  /* `Array<T>` is TypeScript's other spelling of `T[]`, and is turned into one
   * here: two spellings of one type must not become two types. */
  bool isArraySpelling = csTypeNameMatches(name, length, "Array") && *out == TYPE_ARRAY;

  /* `Box<number>` — the arguments are substituted for the declaration's own
   * type parameters, producing a shape whose members are the ones it will
   * really have. */
  if (check(parser, TOKEN_LESS)) {
    int declared = isArraySpelling ? 1 : utilityArity > 0 ? utilityArity : csTypeTypeParamCount(parser->types, *out);
    if (declared == 0) {
      errorAtCurrent(parser, "this type takes no type arguments");
      return false;
    }
    advanceToken(parser);

    TypeId args[CS_MAX_TYPE_PARAMS];
    int argCount = 0;
    do {
      if (argCount >= CS_MAX_TYPE_PARAMS) {
        errorAtCurrent(parser, "a type may take at most four type arguments");
        return false;
      }
      if (!parseTypeUnion(parser, &args[argCount++])) return false;
    } while (matchToken(parser, TOKEN_COMMA));
    consume(parser, TOKEN_GREATER, "expected '>' after the type arguments");
    if (parser->diag->panicMode) return false;

    if (argCount != declared) {
      csDiagnosticError(parser->diag, line, name, length, "'%.*s' takes %d type argument%s and was given %d", length, name, declared, declared == 1 ? "" : "s",
                        argCount);
      return false;
    }
    if (isArraySpelling) {
      *out = csTypeArrayOf(parser->types, args[0]);
    } else if (utilityArity > 0) {
      *out = csTypeUtility(parser->types, name, length, args, argCount, &utilityArity);
    } else {
      *out = csTypeInstantiate(parser->types, *out, args, argCount);
    }
    return true;
  }

  if (utilityArity > 0) {
    csDiagnosticError(parser->diag, line, name, length, "'%.*s' takes %d type argument%s and was given none", length, name, utilityArity, utilityArity == 1 ? "" : "s");
    return false;
  }

  /* `Promise` written bare is a Promise of something the checker cannot see: a
   * generic left uninstantiated would leak its own type variables into the
   * annotation, and nothing outside a declaration should ever see one. */
  if (csTypeTypeParamCount(parser->types, *out) > 0) *out = csTypeInstantiate(parser->types, *out, NULL, 0);
  return true;
}

static bool parseTypeSuffix(Parser *parser, TypeId *out) {
  if (!parseTypePrimary(parser, out)) return false;

  /* `T[]` is an array of them and `T[K]` is what their K holds — told apart by
   * whether anything stands between the brackets. */
  while (check(parser, TOKEN_LEFT_BRACKET)) {
    Lexer probe = parser->lexer;
    bool isArray = csLexerNext(&probe).type == TOKEN_RIGHT_BRACKET;
    advanceToken(parser);

    if (isArray) {
      advanceToken(parser);
      *out = csTypeArrayOf(parser->types, *out);
      continue;
    }

    TypeId key;
    if (!parseTypeUnion(parser, &key)) return false;
    consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the key of an indexed access");
    if (parser->diag->panicMode) return false;
    *out = csTypeIndexedAccess(parser->types, *out, key);
  }
  return true;
}

/* `A | B | null`. The bar is the only infix operator in the type grammar, and
 * it is what makes "a number, or nothing" something a program can write. */
static bool parseTypeUnion(Parser *parser, TypeId *out) {
  if (!parseTypeSuffix(parser, out)) return false;
  if (!check(parser, TOKEN_PIPE)) return true;

  TypeId members[16];
  int count = 0;
  members[count++] = *out;
  while (matchToken(parser, TOKEN_PIPE)) {
    if (count >= (int)(sizeof members / sizeof members[0])) {
      errorAtCurrent(parser, "a union may hold at most 16 types");
      return false;
    }
    if (!parseTypeSuffix(parser, &members[count++])) return false;
  }
  *out = csTypeUnionOf(parser->types, members, count);
  return true;
}

/* The entry point used by annotations and by the declarations below.
 *
 * A conditional binds loosest of all, and its arms are conditionals in turn —
 * `A extends B ? X : C extends D ? Y : Z` reads to the right, which is how a
 * chain of them is written. */
bool parseTypeExpression(Parser *parser, TypeId *out) {
  if (!parseTypeUnion(parser, out)) return false;
  if (!matchToken(parser, TOKEN_EXTENDS)) return true;

  /* Any `infer R` inside the pattern belongs to *this* conditional. Collected
   * from here so a nested one does not take them, and closed below: they are
   * in scope for the true arm and nowhere else. */
  int inferStart = parser->inferCount;

  TypeId extends;
  if (!parseTypeUnion(parser, &extends)) return false;

  consume(parser, TOKEN_QUESTION, "expected '?' and the two types a conditional chooses between");
  if (parser->diag->panicMode) return false;

  TypeId whenTrue;
  if (!parseTypeExpression(parser, &whenTrue)) return false;

  consume(parser, TOKEN_COLON, "expected ':' and what the conditional answers when it does not match");
  if (parser->diag->panicMode) return false;

  TypeId whenFalse;
  if (!parseTypeExpression(parser, &whenFalse)) return false;

  *out = csTypeConditional(parser->types, *out, extends, whenTrue, whenFalse, &parser->inferVars[inferStart], parser->inferCount - inferStart);

  for (int i = inferStart; i < parser->inferCount; i++) csTypeCloseTypeVar(parser->types, parser->inferVars[i]);
  parser->inferCount = inferStart;
  return true;
}
