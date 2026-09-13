/* parser_type.c — `interface` and `type`, the two declarations that describe a
 * shape rather than produce a value.
 *
 * Neither exists at run time: an interface is checked and then erased, which
 * is also what `node --experimental-strip-types` does with the same text. So
 * both parse into nothing — an empty block, the same node a lone `;` makes —
 * and what they leave behind is an entry in the file's TypeRegistry.
 *
 * Both are **contextual**. `type` is an ordinary identifier in every other
 * position and programs already use it as one (`flag.type` in std:cli), so it
 * is a declaration only where a declaration can start and a name and an `=`
 * follow it. `interface` is read the same way for the same reason.
 *
 * A type must be declared before it is named. The parser resolves an
 * annotation the moment it reads it — there is no second pass — so
 * `interface A { b: B }` before `B` exists is an error rather than a forward
 * reference. An interface may name *itself*, because its name is registered
 * before its members are read.
 */
#include <string.h>

#include "compiler/parser_internal.h"

/* `interface Name {` — the shape of the declaration, told apart from an
 * expression that merely begins with the word. */
bool startsInterfaceDeclaration(Parser *parser) {
  /* No registry means the arena could not allocate one, which is reported
   * elsewhere; without it there is nowhere to record a type, so the word is
   * not a declaration. */
  if (parser->types == NULL) return false;
  if (!checkWord(parser, "interface")) return false;
  Lexer probe = parser->lexer;
  return csLexerNext(&probe).type == TOKEN_IDENTIFIER;
}

/* `type Name =`. Two tokens of lookahead, because `type` alone is a name. */
bool startsTypeAlias(Parser *parser) {
  if (parser->types == NULL) return false;
  if (!checkWord(parser, "type")) return false;
  Lexer probe = parser->lexer;
  if (csLexerNext(&probe).type != TOKEN_IDENTIFIER) return false;
  return csLexerNext(&probe).type == TOKEN_EQUAL;
}

/* The name of a type in a position that is not an annotation: after `extends`,
 * after `=` in an alias, and as a member's type. Answers false having already
 * reported, so a caller need only propagate. */
static bool parseTypeReference(Parser *parser, TypeKind *type) {
  if (!matchToken(parser, TOKEN_NULL) && !matchToken(parser, TOKEN_UNDEFINED)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a type name");
  }
  if (parser->diag->panicMode) return false;

  const char *name = parser->previous.start;
  int length = parser->previous.length;
  if (csTypeLookupName(parser->types, name, length, type)) return true;

  const char *why = csTypeRejectedName(name, length);
  if (why != NULL) {
    csDiagnosticError(parser->diag, parser->previous.line, name, length, "%s", why);
  } else {
    csDiagnosticError(parser->diag, parser->previous.line, name, length, "unknown type '%.*s'", length, name);
  }
  return false;
}

/* Reads past a declaration that has already been reported on, so that the
 * statement after it parses cleanly. Without this a duplicate name produces a
 * second, meaningless message about the members. */
static void skipDeclarationBody(Parser *parser) {
  while (!check(parser, TOKEN_EOF) && !check(parser, TOKEN_LEFT_BRACE) && !check(parser, TOKEN_SEMICOLON)) {
    advanceToken(parser);
  }
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
 * A method is recorded as a Function-typed member that remembers what it
 * answers, so `shape.area()` types as a number rather than as a call into the
 * unknown. Its parameters are read and discarded: a call through an interface
 * is checked for what it answers, not for what it takes, because parameter
 * types would need a signature type this lattice does not have yet. */
static bool parseInterfaceMember(Parser *parser, TypeKind owner) {
  consume(parser, TOKEN_IDENTIFIER, "expected a property name");
  if (parser->diag->panicMode) return false;

  InterfaceMember member;
  member.name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &member.length);
  member.type = TYPE_DYNAMIC;
  member.returns = TYPE_DYNAMIC;
  member.isMethod = false;
  member.optional = false;
  int line = parser->previous.line;
  if (member.name == NULL) return false;

  member.optional = matchToken(parser, TOKEN_QUESTION);

  if (matchToken(parser, TOKEN_LEFT_PAREN)) {
    member.isMethod = true;
    member.type = TYPE_FUNCTION;
    int depth = 1;
    while (depth > 0 && !check(parser, TOKEN_EOF)) {
      if (check(parser, TOKEN_LEFT_PAREN)) depth++;
      if (check(parser, TOKEN_RIGHT_PAREN)) depth--;
      advanceToken(parser);
    }
    if (parser->previous.type != TOKEN_RIGHT_PAREN) {
      errorAtCurrent(parser, "expected ')' after the parameters of a method");
      return false;
    }
    /* `area();` with no return type answers something the interface does not
     * say, which is honest rather than an implied `undefined`. */
    if (matchToken(parser, TOKEN_COLON) && !parseTypeReference(parser, &member.returns)) return false;
  } else {
    consume(parser, TOKEN_COLON, "expected ':' and a type after a property name");
    if (parser->diag->panicMode) return false;
    if (!parseTypeReference(parser, &member.type)) return false;
  }

  /* `;` and `,` both separate members, as they do in TypeScript, and the last
   * one may omit it before the closing brace. */
  if (!matchToken(parser, TOKEN_SEMICOLON) && !matchToken(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_BRACE)) {
    errorAtCurrent(parser, "expected ';' after an interface member");
    return false;
  }

  if (!csTypeInterfaceAddMember(parser->types, owner, &member)) {
    csDiagnosticError(parser->diag, line, member.name, member.length, "'%.*s' is declared twice, or the interface has more than %d members", member.length, member.name,
                      CS_MAX_INTERFACE_MEMBERS);
    return false;
  }
  return true;
}

/* `interface Name extends Other { ... }`
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

  TypeKind existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' already names a type", nameLength, name);
    skipDeclarationBody(parser);
    return NULL;
  }

  /* Registered before its members are read, so a member may name it: a linked
   * list is `interface Node { next: Node }` and nothing else. */
  TypeKind declared = csTypeDeclareInterface(parser->types, name, nameLength);
  if (declared == TYPE_ERROR) {
    csDiagnosticError(parser->diag, line, name, nameLength, "a file may declare at most %d interfaces", CS_MAX_INTERFACES);
    return NULL;
  }

  if (matchToken(parser, TOKEN_EXTENDS)) {
    TypeKind parent;
    if (!parseTypeReference(parser, &parent)) return NULL;
    const InterfaceType *base = csTypeInterface(parser->types, parent);
    if (base == NULL) {
      csDiagnosticError(parser->diag, parser->previous.line, parser->previous.start, parser->previous.length,
                        "an interface can only extend an interface, and '%.*s' is not one", parser->previous.length, parser->previous.start);
      return NULL;
    }
    for (int i = 0; i < base->memberCount; i++) {
      if (csTypeInterfaceAddMember(parser->types, declared, &base->members[i])) continue;
      csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' inherits more members than an interface may hold", nameLength, name);
      return NULL;
    }
  }

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' after the interface's name");
  if (parser->diag->panicMode) return NULL;

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    if (!parseInterfaceMember(parser, declared)) return NULL;
  }
  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the interface's members");
  if (parser->diag->panicMode) return NULL;

  /* An interface is erased, so it compiles to what a lone `;` compiles to. */
  return csAstBlock(parser->arena, line);
}

/* `type Name = number;` and `type Name = { x: number };`
 *
 * The second registers an interface under the alias's name: an object type
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

  TypeKind existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' already names a type", nameLength, name);
    skipDeclarationBody(parser);
    return NULL;
  }
  consume(parser, TOKEN_EQUAL, "expected '=' after the name of a type");
  if (parser->diag->panicMode) return NULL;

  TypeKind aliased;
  if (check(parser, TOKEN_LEFT_BRACE)) {
    aliased = csTypeDeclareInterface(parser->types, name, nameLength);
    if (aliased == TYPE_ERROR) {
      csDiagnosticError(parser->diag, line, name, nameLength, "a file may declare at most %d interfaces", CS_MAX_INTERFACES);
      return NULL;
    }
    advanceToken(parser); /* `{` */
    while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
      if (!parseInterfaceMember(parser, aliased)) return NULL;
    }
    consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the members of an object type");
    if (parser->diag->panicMode) return NULL;
  } else {
    if (!parseTypeReference(parser, &aliased)) return NULL;
    if (!csTypeDeclareAlias(parser->types, name, nameLength, aliased)) {
      csDiagnosticError(parser->diag, line, name, nameLength, "a file may declare at most %d type aliases", CS_MAX_TYPE_ALIASES);
      return NULL;
    }
  }

  consume(parser, TOKEN_SEMICOLON, "expected ';' after a type alias");
  if (parser->diag->panicMode) return NULL;
  return csAstBlock(parser->arena, line);
}
