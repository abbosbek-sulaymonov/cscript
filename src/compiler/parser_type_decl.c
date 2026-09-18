/* parser_type_decl.c — `interface` and `type`, the two declarations that
 * describe a shape rather than produce a value.
 *
 * Neither exists at run time: both are checked and then erased, which is also
 * what `node --experimental-strip-types` does with the same text. So both
 * parse into nothing — an empty block, the same node a lone `;` makes — and
 * what they leave behind is an entry in the file's type table.
 *
 * Both are **contextual**. `type` is an ordinary identifier in every other
 * position and programs already use it as one (`flag.type` in std:cli), so it
 * is a declaration only where a declaration can start and a name and an `=`
 * follow it.
 *
 * A type must be declared before it is named. The parser resolves an
 * annotation the moment it reads it — there is no second pass — so
 * `interface A { b: B }` before `B` exists is an error rather than a forward
 * reference. An interface may name *itself*, because its name is registered
 * before its members are read.
 *
 * What may be written after a `:` — including a mapped type — is the grammar
 * in parser_type.c.
 */
#include <string.h>

#include "compiler/parser_internal.h"
#include "compiler/type_internal.h"

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
  /* Blank first: a field added to TypeMember later must not be read unset. */
  memset(&member, 0, sizeof member);
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
    if (!parseTypeParameterList(parser, params, &paramCount, &requiredCount, &hasRest, "expected ')' after the parameters of a method")) return false;

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
/* `{ [K in keyof T]?: T[K] }` — told from an ordinary body by the `in` after
 * the name in brackets, which nothing else in a type can be.
 *
 * `readonly` and `?` may each be added with the modifier alone or taken away
 * with `-` in front of it, as TypeScript writes them; without either, what the
 * source member said is what the mapped one says. */
static bool startsMappedType(Parser *parser) {
  Lexer probe = parser->lexer;
  Token first = csLexerNext(&probe);
  if (first.type == TOKEN_MINUS) first = csLexerNext(&probe);

  /* `{ readonly [K in …] }` and `{ -readonly [K in …] }`. */
  if (first.type == TOKEN_IDENTIFIER && nameIs(first.start, first.length, "readonly")) first = csLexerNext(&probe);
  if (first.type != TOKEN_LEFT_BRACKET) return false;
  if (csLexerNext(&probe).type != TOKEN_IDENTIFIER) return false;
  /* `in` is the operator's keyword, and a mapped type borrows it. */
  return csLexerNext(&probe).type == TOKEN_IN;
}

/* Reads `?`, `-?`, `readonly` or `-readonly`, answering what it means. */
static TypeModifier readModifier(Parser *parser, TokenType marker, const char *word) {
  if (check(parser, TOKEN_MINUS)) {
    Lexer probe = parser->lexer;
    Token next = csLexerNext(&probe);
    bool matches = word != NULL ? (next.type == TOKEN_IDENTIFIER && nameIs(next.start, next.length, word)) : next.type == marker;
    if (!matches) return MODIFIER_KEEP;
    advanceToken(parser);
    advanceToken(parser);
    return MODIFIER_REMOVE;
  }
  if (word != NULL) {
    if (!checkWord(parser, word)) return MODIFIER_KEEP;
    advanceToken(parser);
    return MODIFIER_ADD;
  }
  return matchToken(parser, marker) ? MODIFIER_ADD : MODIFIER_KEEP;
}

/* The whole of a mapped type, answering the type it describes. */
static bool parseMappedType(Parser *parser, TypeId *out, const char *name, int nameLength) {
  advanceToken(parser); /* `{` */

  TypeModifier readonlyMode = readModifier(parser, TOKEN_EOF, "readonly");

  consume(parser, TOKEN_LEFT_BRACKET, "expected '[' to open a mapped type");
  consume(parser, TOKEN_IDENTIFIER, "expected the name a mapped type binds");
  if (parser->diag->panicMode) return false;

  int boundLength;
  const char *bound = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &boundLength);
  if (bound == NULL) return false;

  if (!matchToken(parser, TOKEN_IN)) {
    errorAtCurrent(parser, "expected 'in' and the names to map over");
    return false;
  }

  /* The variable is in scope for the value type and nowhere else, which is the
   * same rule a declaration's own type parameters follow. */
  TypeId variable = csTypeDeclareTypeVar(parser->types, bound, boundLength);
  if (variable == TYPE_ERROR) {
    errorAtCurrent(parser, "this file declares more types than the checker can hold");
    return false;
  }

  TypeId keys;
  bool ok = parseTypeExpression(parser, &keys);
  if (ok) {
    consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the names a mapped type maps over");
    ok = !parser->diag->panicMode;
  }

  TypeModifier optionalMode = MODIFIER_KEEP;
  TypeId value = TYPE_DYNAMIC;
  if (ok) {
    optionalMode = readModifier(parser, TOKEN_QUESTION, NULL);
    consume(parser, TOKEN_COLON, "expected ':' and what each member holds");
    ok = !parser->diag->panicMode && parseTypeExpression(parser, &value);
  }
  if (ok) {
    matchToken(parser, TOKEN_SEMICOLON);
    matchToken(parser, TOKEN_COMMA);
    consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after a mapped type");
    ok = !parser->diag->panicMode;
  }

  csTypeCloseTypeVar(parser->types, variable);
  if (!ok) return false;

  /* `{ [K in keyof T]: … }` — what the keys were taken from, so that what T
   * said about each member survives a mapping that does not say otherwise. */
  const CompositeType *keySource = csTypeComposite(parser->types, keys);
  TypeId source = keySource != NULL && keySource->kind == COMPOSITE_KEYOF ? keySource->inner : TYPE_DYNAMIC;

  *out = csTypeMapped(parser->types, keys, variable, value, optionalMode, readonlyMode, name, nameLength, source);
  return true;
}

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

  /* Two pointers to one name. The interned copy outlives the parse and is
   * what the type table keeps; the token points into the source, which is
   * where a diagnostic cuts its excerpt from — passing the interned one made
   * the excerpt a slice of the arena. */
  Token named = parser->previous;
  int nameLength;
  const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &nameLength);
  if (name == NULL) return NULL;

  TypeId existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' already names a type", nameLength, name);
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
    csDiagnosticError(parser->diag, line, named.start, named.length, "this file declares more types than the checker can hold");
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
      csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' inherits a member it already has, or more than the checker can hold", nameLength, name);
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

  /* Two pointers to one name. The interned copy outlives the parse and is
   * what the type table keeps; the token points into the source, which is
   * where a diagnostic cuts its excerpt from — passing the interned one made
   * the excerpt a slice of the arena. */
  Token named = parser->previous;
  int nameLength;
  const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &nameLength);
  if (name == NULL) return NULL;

  TypeId existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' already names a type", nameLength, name);
    skipDeclarationBody(parser);
    return NULL;
  }

  TypeId params[CS_MAX_TYPE_PARAMS];
  int paramCount = 0;
  if (!parseTypeParams(parser, params, &paramCount)) return NULL;

  consume(parser, TOKEN_EQUAL, "expected '=' after the name of a type");
  if (parser->diag->panicMode) return NULL;

  TypeId aliased;
  if (check(parser, TOKEN_LEFT_BRACE) && startsMappedType(parser)) {
    /* `type Partial<T> = { [K in keyof T]?: T[K] }` — the alias names the
     * mapping rather than a shape, because there is no shape until there is a
     * T. Instantiating the alias substitutes through the mapping and evaluates
     * what comes out. */
    if (!parseMappedType(parser, &aliased, name, nameLength)) return NULL;
    if (paramCount > 0) csTypeSetTypeParams(parser->types, aliased, params, paramCount);
    if (!csTypeDeclareAlias(parser->types, name, nameLength, aliased)) {
      csDiagnosticError(parser->diag, line, named.start, named.length, "a file may declare at most %d type aliases", CS_MAX_TYPE_ALIASES);
      return NULL;
    }
    closeTypeParams(parser, params, paramCount);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after a type alias");
    if (parser->diag->panicMode) return NULL;
    return csAstBlock(parser->arena, line);
  }

  if (check(parser, TOKEN_LEFT_BRACE)) {
    aliased = csTypeDeclareInterface(parser->types, name, nameLength);
    if (aliased == TYPE_ERROR) {
      csDiagnosticError(parser->diag, line, named.start, named.length, "this file declares more types than the checker can hold");
      return NULL;
    }
    csTypeSetTypeParams(parser->types, aliased, params, paramCount);
    if (!parseInterfaceBody(parser, aliased)) return NULL;
  } else {
    if (!parseTypeExpression(parser, &aliased)) return NULL;

    /* `type Drop<T, U> = T extends U ? never : T` — the parameters belong to
     * the computed form, which is what an instantiation substitutes through.
     *
     * Only for a computed form, and that is the whole of the condition. Those
     * are interned on the pieces they are built from, and those pieces include
     * this declaration's own type variables, so the composite is this alias's
     * and no one else's. An ordinary type is interned on its *shape* — every
     * `number[]` in the file is one composite — so tagging one with parameters
     * would make every other use of it generic. */
    if (paramCount > 0 && (csTypeIs(parser->types, aliased, COMPOSITE_CONDITIONAL) || csTypeIs(parser->types, aliased, COMPOSITE_KEYOF) ||
                           csTypeIs(parser->types, aliased, COMPOSITE_INDEXED) || csTypeIs(parser->types, aliased, COMPOSITE_MAPPED))) {
      csTypeSetTypeParams(parser->types, aliased, params, paramCount);
    }
    if (!csTypeDeclareAlias(parser->types, name, nameLength, aliased)) {
      csDiagnosticError(parser->diag, line, named.start, named.length, "a file may declare at most %d type aliases", CS_MAX_TYPE_ALIASES);
      return NULL;
    }
  }
  closeTypeParams(parser, params, paramCount);

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a type alias");
  if (parser->diag->panicMode) return NULL;
  return csAstBlock(parser->arena, line);
}
