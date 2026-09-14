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
#include <string.h>

#include "compiler/parser_internal.h"

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
static bool parseParameterTypes(Parser *parser, TypeId *params, int *paramCount, int *requiredCount, bool *hasRest, const char *what) {
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
  if (!parseParameterTypes(parser, params, &paramCount, &requiredCount, &hasRest, "expected ')' after the parameters of a function type")) return false;

  consume(parser, TOKEN_ARROW, "expected '=>' and a result type");
  if (parser->diag->panicMode) return false;

  TypeId result;
  if (!parseTypeUnion(parser, &result)) return false;
  *out = csTypeFunctionOf(parser->types, params, paramCount, requiredCount, hasRest, result);
  return true;
}

static bool parseTypePrimary(Parser *parser, TypeId *out) {
  if (check(parser, TOKEN_LEFT_PAREN)) {
    if (startsFunctionType(parser)) return parseFunctionType(parser, out);
    advanceToken(parser);
    if (!parseTypeUnion(parser, out)) return false;
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the type");
    return !parser->diag->panicMode;
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
  if (!csTypeLookupName(parser->types, name, length, out)) {
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

  /* `Box<number>` — the arguments are substituted for the declaration's own
   * type parameters, producing a shape whose members are the ones it will
   * really have. */
  if (check(parser, TOKEN_LESS)) {
    int declared = csTypeTypeParamCount(parser->types, *out);
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
    *out = csTypeInstantiate(parser->types, *out, args, argCount);
  }
  return true;
}

static bool parseTypeSuffix(Parser *parser, TypeId *out) {
  if (!parseTypePrimary(parser, out)) return false;

  /* `T[]`, and `T[][]` for an array of them. */
  while (check(parser, TOKEN_LEFT_BRACKET)) {
    Lexer probe = parser->lexer;
    if (csLexerNext(&probe).type != TOKEN_RIGHT_BRACKET) break;
    advanceToken(parser);
    advanceToken(parser);
    *out = csTypeArrayOf(parser->types, *out);
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

/* The entry point used by annotations and by the declarations below. */
bool parseTypeExpression(Parser *parser, TypeId *out) {
  return parseTypeUnion(parser, out);
}

/* Reads past a declaration that has already been reported on, so that the
 * statement after it parses cleanly. Without this a duplicate name produces a
 * second, meaningless message about the members. */
static void skipDeclarationBody(Parser *parser) {
  while (!check(parser, TOKEN_EOF) && !check(parser, TOKEN_LEFT_BRACE) && !check(parser, TOKEN_SEMICOLON)) advanceToken(parser);
  if (matchToken(parser, TOKEN_SEMICOLON)) return;
  if (!check(parser, TOKEN_LEFT_BRACE)) return;

  int depth = 0;
  do {
    if (check(parser, TOKEN_LEFT_BRACE)) depth++;
    if (check(parser, TOKEN_RIGHT_BRACE)) depth--;
    advanceToken(parser);
  } while (depth > 0 && !check(parser, TOKEN_EOF));
  matchToken(parser, TOKEN_SEMICOLON);
}

/* One member: `x: number;`, `x?: number,` or `area(): number;`.
 *
 * A method is recorded as a member whose type is a function type, so a call
 * through it is checked by the same rule as a call through a variable rather
 * than by a second rule for interfaces. */
static bool parseInterfaceMember(Parser *parser, TypeId owner) {
  consume(parser, TOKEN_IDENTIFIER, "expected a property name");
  if (parser->diag->panicMode) return false;

  TypeMember member;
  member.name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &member.length);
  member.type = TYPE_DYNAMIC;
  member.optional = false;
  int line = parser->previous.line;
  if (member.name == NULL) return false;

  member.optional = matchToken(parser, TOKEN_QUESTION);

  if (check(parser, TOKEN_LEFT_PAREN)) {
    advanceToken(parser);
    TypeId params[CS_MAX_TYPE_PARAMS * 8];
    int paramCount;
    int requiredCount;
    bool hasRest;
    if (!parseParameterTypes(parser, params, &paramCount, &requiredCount, &hasRest, "expected ')' after the parameters of a method")) return false;

    /* `area();` with no result type answers something the interface does not
     * say, which is honest rather than an implied `undefined`. */
    TypeId result = TYPE_DYNAMIC;
    if (matchToken(parser, TOKEN_COLON) && !parseTypeExpression(parser, &result)) return false;
    member.type = csTypeFunctionOf(parser->types, params, paramCount, requiredCount, hasRest, result);
  } else {
    consume(parser, TOKEN_COLON, "expected ':' and a type after a property name");
    if (parser->diag->panicMode) return false;
    if (!parseTypeExpression(parser, &member.type)) return false;
  }

  /* `;` and `,` both separate members, as they do in TypeScript, and the last
   * one may omit it before the closing brace. */
  if (!matchToken(parser, TOKEN_SEMICOLON) && !matchToken(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_BRACE)) {
    errorAtCurrent(parser, "expected ';' after an interface member");
    return false;
  }

  if (!csTypeAddMember(parser->types, owner, &member)) {
    csDiagnosticError(parser->diag, line, member.name, member.length, "'%.*s' is declared twice, or this file declares more members than the checker can hold",
                      member.length, member.name);
    return false;
  }
  return true;
}

/* The members between `{` and `}`. */
static bool parseInterfaceBody(Parser *parser, TypeId declared) {
  consume(parser, TOKEN_LEFT_BRACE, "expected '{' before the members");
  if (parser->diag->panicMode) return false;
  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    if (!parseInterfaceMember(parser, declared)) return false;
  }
  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the members");
  return !parser->diag->panicMode;
}

/* `interface Name<T> extends Other { ... }`
 *
 * `extends` copies the members across rather than recording a link, because
 * assignability here is structural: an interface is satisfied by what a value
 * has, so what an inherited member buys is exactly its presence in the list. */
AstNode *parseInterfaceDeclaration(Parser *parser) {
  int line = parser->current.line;
  advanceToken(parser); /* the word `interface` */
  consume(parser, TOKEN_IDENTIFIER, "expected a name after 'interface'");
  if (parser->diag->panicMode) return NULL;

  int nameLength;
  const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &nameLength);
  if (name == NULL) return NULL;

  TypeId existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' already names a type", nameLength, name);
    skipDeclarationBody(parser);
    return NULL;
  }

  /* The type parameters are declared before the shape, so that `interface
   * Box<T>` may say `T` in its own members. */
  TypeId params[CS_MAX_TYPE_PARAMS];
  int paramCount = 0;
  if (!parseTypeParams(parser, params, &paramCount)) return NULL;

  /* Registered before its members are read, so a member may name it: a linked
   * list is `interface Node { next?: Node }` and nothing else. */
  TypeId declared = csTypeDeclareInterface(parser->types, name, nameLength);
  if (declared == TYPE_ERROR) {
    csDiagnosticError(parser->diag, line, name, nameLength, "this file declares more types than the checker can hold");
    return NULL;
  }
  csTypeSetTypeParams(parser->types, declared, params, paramCount);

  if (matchToken(parser, TOKEN_EXTENDS)) {
    TypeId parent;
    if (!parseTypeExpression(parser, &parent)) return NULL;
    const CompositeType *base = csTypeComposite(parser->types, parent);
    if (base == NULL || base->kind != COMPOSITE_INTERFACE) {
      csDiagnosticError(parser->diag, parser->previous.line, parser->previous.start, parser->previous.length,
                        "an interface can only extend an interface, and '%.*s' is not one", parser->previous.length, parser->previous.start);
      return NULL;
    }
    int start = base->memberStart;
    int count = base->memberCount;
    for (int i = 0; i < count; i++) {
      TypeMember inherited = parser->types->members[start + i];
      if (csTypeAddMember(parser->types, declared, &inherited)) continue;
      csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' inherits a member it already has, or more than the checker can hold", nameLength, name);
      return NULL;
    }
  }

  if (!parseInterfaceBody(parser, declared)) return NULL;
  closeTypeParams(parser, params, paramCount);

  /* An interface is erased, so it compiles to what a lone `;` compiles to. */
  return csAstBlock(parser->arena, line);
}

/* `type Name = number;`, `type Name = { x: number };`, `type Box<T> = …`
 *
 * The object form registers an interface under the alias's name: a shape
 * written inline and an interface declared with a name are the same thing
 * here, and giving them one representation means the checker has one rule. */
AstNode *parseTypeAlias(Parser *parser) {
  int line = parser->current.line;
  advanceToken(parser); /* the word `type` */
  consume(parser, TOKEN_IDENTIFIER, "expected a name after 'type'");
  if (parser->diag->panicMode) return NULL;

  int nameLength;
  const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &nameLength);
  if (name == NULL) return NULL;

  TypeId existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' already names a type", nameLength, name);
    skipDeclarationBody(parser);
    return NULL;
  }

  TypeId params[CS_MAX_TYPE_PARAMS];
  int paramCount = 0;
  if (!parseTypeParams(parser, params, &paramCount)) return NULL;

  consume(parser, TOKEN_EQUAL, "expected '=' after the name of a type");
  if (parser->diag->panicMode) return NULL;

  TypeId aliased;
  if (check(parser, TOKEN_LEFT_BRACE)) {
    aliased = csTypeDeclareInterface(parser->types, name, nameLength);
    if (aliased == TYPE_ERROR) {
      csDiagnosticError(parser->diag, line, name, nameLength, "this file declares more types than the checker can hold");
      return NULL;
    }
    csTypeSetTypeParams(parser->types, aliased, params, paramCount);
    if (!parseInterfaceBody(parser, aliased)) return NULL;
  } else {
    if (!parseTypeExpression(parser, &aliased)) return NULL;
    if (!csTypeDeclareAlias(parser->types, name, nameLength, aliased)) {
      csDiagnosticError(parser->diag, line, name, nameLength, "a file may declare at most %d type aliases", CS_MAX_TYPE_ALIASES);
      return NULL;
    }
  }
  closeTypeParams(parser, params, paramCount);

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a type alias");
  if (parser->diag->panicMode) return NULL;
  return csAstBlock(parser->arena, line);
}
