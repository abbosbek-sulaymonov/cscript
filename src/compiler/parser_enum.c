/* parser_enum.c — `enum`, which is a declaration and an object at once.
 *
 * Everything else the type grammar declares is erased: an `interface` compiles
 * to nothing, a `type` alias compiles to nothing, an annotation is read and
 * dropped. An enum cannot be, because `Color.Red` has to answer something at
 * run time — so this is the one construct here that is both a type and a
 * value, and the one place the two are declared together.
 *
 * What it compiles to is an object, which is what TypeScript emits too:
 *
 *     enum Color { Red, Green }   ->   const Color = { Red: 0, Green: 1,
 *                                                      "0": "Red", "1": "Green" };
 *
 * The numbers-to-names half is there for the same reason it is there in
 * TypeScript: `Color[value]` is how a program prints one. A string enum has no
 * reverse half, because a string member's value could collide with a name.
 *
 * The desugaring is done here rather than in the compiler on purpose. An enum
 * has no run-time behaviour an object literal does not already have, so giving
 * it an opcode, a value type and a collector case would be three new things
 * that answer a question the object literal answers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cscript/ast_build.h"
#include "cscript/parser.h"
#include "compiler/parser_internal.h"
#include "compiler/type_internal.h"

#define CS_MAX_ENUM_MEMBERS 128

/* `enum Name {` — told apart from an expression that merely begins with the
 * word, the same way `interface` and `type` are. */
bool startsEnumDeclaration(Parser *parser) {
  if (parser->types == NULL) return false;
  if (!checkWord(parser, "enum")) return false;
  Lexer probe = parser->lexer;
  return csLexerNext(&probe).type == TOKEN_IDENTIFIER;
}

/* The text of a whole number, for the key of the reverse entry. `-0` and
 * anything fractional never reach this: a reverse entry is only made for a
 * value that came from counting. */
static AstNode *wholeNumberKey(Parser *parser, int line, double value) {
  char digits[32];
  int written = snprintf(digits, sizeof digits, "%.0f", value);
  if (written <= 0 || written >= (int)sizeof digits) return NULL;
  return csAstString(parser->arena, line, digits, written);
}

/* One `= …` initialiser, which may be a number, a string, or a negated
 * number. Anything else is a value the declaration cannot settle, and an enum
 * whose members are not constants is one nothing downstream could use. */
static bool parseEnumValue(Parser *parser, bool *isString, double *number, const char **text, int *textLength) {
  bool negated = matchToken(parser, TOKEN_MINUS);

  if (matchToken(parser, TOKEN_NUMBER)) {
    *isString = false;
    *number = strtod(parser->previous.start, NULL);
    if (negated) *number = -*number;
    return true;
  }
  if (!negated && matchToken(parser, TOKEN_STRING)) {
    *isString = true;
    /* The lexer keeps the quotes; the member's value is what is between. */
    *text = parser->previous.start + 1;
    *textLength = parser->previous.length - 2;
    return true;
  }

  errorAtCurrent(parser, "an enum member is a number or a string written out here");
  return false;
}

/* `enum Name { A, B = 2, C }`
 *
 * Answers the `const` declaration it compiles to, having also declared the
 * type `Name` — which is what the members' values are, rather than what the
 * object is. That split is TypeScript's: `Name` in a type position is the
 * value, and the object is what the name holds. */
AstNode *parseEnumDeclaration(Parser *parser) {
  int line = parser->current.line;
  advanceToken(parser); /* the word `enum` */
  consume(parser, TOKEN_IDENTIFIER, "expected a name after 'enum'");
  if (parser->diag->panicMode) return NULL;

  /* Two pointers to one name: the interned copy the type table keeps, and the
   * token, which points into the source a diagnostic cuts its excerpt from. */
  Token named = parser->previous;
  int nameLength;
  const char *name = csAstInternName(parser->arena, parser->previous.start, parser->previous.length, &nameLength);
  if (name == NULL) return NULL;

  TypeId existing;
  if (csTypeLookupName(parser->types, name, nameLength, &existing)) {
    csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' already names a type", nameLength, name);
    return NULL;
  }

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' to open the enum body");
  if (parser->diag->panicMode) return NULL;

  AstNode *object = csAstObjectLiteral(parser->arena, line);
  if (object == NULL) return NULL;

  /* The shape the *name* holds: one member per enum member, so `Color.Red`
   * reads and `Color.Rd` does not. It carries the enum's name for its
   * messages, and nothing can reach it by that name — the alias declared at
   * the end of this function is what `Color` means in a type position, and an
   * alias is looked up first. */
  TypeId shape = csTypeDeclareInterface(parser->types, name, nameLength);

  /* What the members are, as a type. A string enum's is exact — a union of its
   * values, because this lattice has literal string types. A numeric one's is
   * `number`, because it has no literal number types, so a numeric enum says
   * less about a parameter than a string enum does. */
  TypeId literals[CS_MAX_ENUM_MEMBERS];
  int literalCount = 0;
  bool anyNumeric = false;

  double counter = 0;
  int memberCount = 0;

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    if (!consumePropertyName(parser, "expected an enum member name")) return NULL;
    if (parser->diag->panicMode) return NULL;

    int memberLength = parser->previous.length;
    const char *member = csAstInternName(parser->arena, parser->previous.start, memberLength, &memberLength);
    if (member == NULL) return NULL;
    if (memberCount >= CS_MAX_ENUM_MEMBERS) {
      csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' has more members than the checker can hold", nameLength, name);
      return NULL;
    }
    memberCount++;

    bool isString = false;
    double number = counter;
    const char *text = NULL;
    int textLength = 0;
    const char *reverseName = NULL;
    int reverseLength = 0;
    if (matchToken(parser, TOKEN_EQUAL)) {
      if (!parseEnumValue(parser, &isString, &number, &text, &textLength)) return NULL;
    }

    AstNode *key = csAstString(parser->arena, parser->previous.line, member, memberLength);
    AstNode *value = isString ? csAstString(parser->arena, line, text, textLength) : csAstNumber(parser->arena, line, number);
    if (key == NULL || value == NULL) return NULL;
    csAstObjectLiteralAdd(parser->arena, object, key, value);

    /* `A = 2, B` — counting resumes from what was written down, which is the
     * rule TypeScript follows and the reason a member after a string one must
     * say its own value. */
    TypeId held = TYPE_NUMBER;
    if (isString) {
      held = csTypeLiteral(parser->types, text, textLength);
      if (literalCount < CS_MAX_ENUM_MEMBERS) literals[literalCount++] = held;
    } else {
      anyNumeric = true;
      counter = number + 1;

      /* The number back to the name, which is how a program prints one. Only
       * for a whole number: a fractional key is not what `Color[n]` looks up. */
      if (number == (double)(long long)number) {
        AstNode *reverseKey = wholeNumberKey(parser, line, number);
        AstNode *reverseValue = csAstString(parser->arena, line, member, memberLength);
        if (reverseKey != NULL && reverseValue != NULL) {
          csAstObjectLiteralAdd(parser->arena, object, reverseKey, reverseValue);
          reverseName = reverseKey->as.string.chars;
          reverseLength = reverseKey->as.string.length;
        }
      }
    }

    if (shape != TYPE_ERROR) {
      TypeMember entry;
      memset(&entry, 0, sizeof entry);
      entry.name = member;
      entry.length = memberLength;
      entry.type = held;
      entry.readonly = true;
      csTypeAddMember(parser->types, shape, &entry);

      /* And the reverse entry, which is a member like any other: `Color[0]` is
       * the string "Red", exactly, because the name is a literal type. */
      if (reverseName != NULL) {
        TypeMember back;
        memset(&back, 0, sizeof back);
        back.name = reverseName;
        back.length = reverseLength;
        back.type = csTypeLiteral(parser->types, member, memberLength);
        back.readonly = true;
        csTypeAddMember(parser->types, shape, &back);
      }
    }

    if (check(parser, TOKEN_RIGHT_BRACE)) break;
    if (!matchToken(parser, TOKEN_COMMA)) {
      /* `A = 1 + 2` gets here, and the reason is worth saying: a member's
       * value has to be settled where it is written, because the whole of what
       * an enum is for is a name for a constant. */
      errorAtCurrent(parser, "expected ',' or '}' — an enum member's value is written out, not computed");
      return NULL;
    }
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the enum members");
  if (parser->diag->panicMode) return NULL;
  if (memberCount == 0) {
    csDiagnosticError(parser->diag, line, named.start, named.length, "'%.*s' has no members, and an enum with none names nothing", nameLength, name);
    return NULL;
  }
  matchToken(parser, TOKEN_SEMICOLON);

  TypeId valueType = anyNumeric || literalCount == 0 ? TYPE_NUMBER : csTypeUnionOf(parser->types, literals, literalCount);
  if (anyNumeric && literalCount > 0) {
    /* Mixed, which TypeScript allows: what a member holds is either, and this
     * lattice can say that exactly. */
    literals[literalCount] = TYPE_NUMBER;
    valueType = csTypeUnionOf(parser->types, literals, literalCount + 1);
  }
  csTypeDeclareAlias(parser->types, name, nameLength, valueType);

  return csAstVarDecl(parser->arena, line, name, nameLength, object, true, shape == TYPE_ERROR ? TYPE_DYNAMIC : shape, shape != TYPE_ERROR);
}
