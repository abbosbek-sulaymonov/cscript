/* parser_class.c — the class body.
 *
 * Fields, methods, accessors, statics, static blocks, private members and
 * computed names — in source order, because the order the initialisers run in
 * is observable and a class that reorders them is a different program.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"

AstNode *parseClass(Parser *parser) {
  int line = parser->previous.line;

  consume(parser, TOKEN_IDENTIFIER, "expected a class name");
  if (parser->diag->panicMode) return NULL;
  return parseClassBody(parser, line, parser->previous.start, parser->previous.length);
}

/* Everything after the name, which a class expression has none of. */
AstNode *parseClassBody(Parser *parser, int line, const char *name, int nameLength) {
  const char *superName = NULL;
  int superLength = 0;
  if (matchToken(parser, TOKEN_EXTENDS)) {
    consume(parser, TOKEN_IDENTIFIER, "expected a superclass name after 'extends'");
    if (parser->diag->panicMode) return NULL;
    superName = parser->previous.start;
    superLength = parser->previous.length;
    if (superLength == nameLength && memcmp(superName, name, (size_t)superLength) == 0) {
      errorAtCurrent(parser, "a class cannot extend itself");
      return NULL;
    }
  }

  AstNode *node = csAstClass(parser->arena, line, name, nameLength, superName, superLength);

  consume(parser, TOKEN_LEFT_BRACE, "expected '{' to open the class body");
  if (parser->diag->panicMode) return NULL;

  while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
    /* A stray ';' between members is legal and means nothing. */
    if (matchToken(parser, TOKEN_SEMICOLON)) continue;

    bool isStatic = matchToken(parser, TOKEN_STATIC);
    if (isStatic && check(parser, TOKEN_LEFT_BRACE)) {
      /* `static { … }` runs once, where the class is built, with `this` bound
       * to the class. That is a method with no name and no parameters, so it
       * is stored as one. */
      int blockLine = parser->current.line;
      advanceToken(parser);

      AstNode *block = csAstFunction(parser->arena, blockLine, " static", 7);
      if (block == NULL) return NULL;
      block->as.function.isMethod = true;
      block->as.function.body = parseBlock(parser);
      if (block->as.function.body == NULL) return NULL;

      csAstClassAddMember(parser->arena, node, block, true, MEMBER_STATIC_BLOCK);
      continue;
    }

    /* `async m() {}` — an `async` followed by another name, rather than by
     * `(`, is the modifier and not the method's own name. */
    bool isAsyncMember = false;
    if (checkWord(parser, "async")) {
      Lexer probe = parser->lexer;
      Token next = csLexerNext(&probe);
      if (next.type != TOKEN_LEFT_PAREN && next.type != TOKEN_EQUAL && next.type != TOKEN_SEMICOLON && next.type != TOKEN_COLON) {
        advanceToken(parser);
        isAsyncMember = true;
      }
    }

    /* `*next() {}` — a generator method. Read before the name, like `async`. */
    bool isGeneratorMember = matchToken(parser, TOKEN_STAR);

    /* `[key]() {}` — the name is an expression, evaluated where the class is
     * built. The member still needs a name to compile under; this one is
     * unwritable and never reaches the class. */
    AstNode *computedKey = NULL;
    const char *memberName;
    int memberLength;
    int memberLine = parser->current.line;

    if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
      computedKey = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after a computed name");
      if (computedKey == NULL || parser->diag->panicMode) return NULL;
      memberName = " computed";
      memberLength = 9;
    } else {
      if (!consumePropertyName(parser, "expected a member name")) return NULL;
      if (parser->diag->panicMode) return NULL;
      memberName = parser->previous.start;
      memberLength = parser->previous.length;
      memberLine = parser->previous.line;
    }

    /* `get x() {}` — an accessor, unless `get` is the member's own name, which
     * it is whenever `(` follows it directly. */
    ClassMemberKind memberKind = MEMBER_METHOD;
    /* `get` is only a modifier when a name or a computed key follows it.
     * `get() {}` is a method called `get`, and `get = ...` is a field called
     * `get` — the same rule the object-literal parser follows, and without it
     * a field may not be named after an accessor keyword. */
    if ((nameIs(memberName, memberLength, "get") || nameIs(memberName, memberLength, "set")) && !check(parser, TOKEN_LEFT_PAREN) && !check(parser, TOKEN_EQUAL) &&
        !check(parser, TOKEN_SEMICOLON)) {
      memberKind = memberName[0] == 'g' ? MEMBER_GETTER : MEMBER_SETTER;

      /* `get [key]() {}` — the accessor's name may be computed too. */
      if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
        computedKey = parseExpression(parser);
        consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after a computed name");
        if (computedKey == NULL || parser->diag->panicMode) return NULL;
        memberName = " computed";
        memberLength = 9;
      } else {
        if (!consumePropertyName(parser, "expected a name after 'get' or 'set'")) {
          return NULL;
        }
        memberName = parser->previous.start;
        memberLength = parser->previous.length;
        memberLine = parser->previous.line;
      }
    }

    if (check(parser, TOKEN_LEFT_PAREN)) {
      /* The constructor is named after its class, because that is what an
       * arity error or a stack frame should say: `Dog expects 1 argument`
       * rather than `constructor expects 1 argument`. */
      bool isConstructor = nameIs(memberName, memberLength, "constructor");
      if (isConstructor && isAsyncMember) {
        errorAtCurrent(parser, "a constructor cannot be async");
        return NULL;
      }
      parser->pendingAsync = isAsyncMember;
      parser->pendingGenerator = isGeneratorMember;
      AstNode *method = parseFunctionRest(parser, memberLine, isConstructor ? name : memberName, isConstructor ? nameLength : memberLength, true);
      if (method == NULL) return NULL;

      if (isConstructor) {
        if (isStatic) {
          errorAtCurrent(parser, "a constructor cannot be static");
          return NULL;
        }
        if (node->as.classDecl.constructor != NULL) {
          errorAtCurrent(parser, "a class can only have one constructor");
          return NULL;
        }
        node->as.classDecl.constructor = method;
      } else {
        csAstClassAddMember(parser->arena, node, method, isStatic, memberKind);
        node->as.classDecl.members[node->as.classDecl.memberCount - 1].computedKey = computedKey;
      }
      continue;
    }

    TypeKind fieldType;
    bool annotated;
    if (!parseTypeAnnotation(parser, &fieldType, &annotated)) return NULL;

    AstNode *initializer = NULL;
    if (matchToken(parser, TOKEN_EQUAL)) {
      initializer = parseExpression(parser);
      if (initializer == NULL) return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the field declaration");
    if (parser->diag->panicMode) return NULL;

    csAstClassAddField(parser->arena, node, memberName, memberLength, initializer, fieldType, annotated, isStatic);
    node->as.classDecl.fields[node->as.classDecl.fieldCount - 1].computedKey = computedKey;
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the class body");
  if (parser->diag->panicMode) return NULL;
  return node;
}

/* The `{ a, b as c }` shared by import and export lists. */
