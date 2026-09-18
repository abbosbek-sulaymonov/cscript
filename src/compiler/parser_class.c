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
  const char *name = parser->previous.start;
  int nameLength = parser->previous.length;

  /* `class Box<T>` — read before anything else in the declaration, because the
   * shape is registered next and every member after it may say `T`. A class
   * expression has no name to hang them off and takes none. */
  TypeId params[CS_MAX_TYPE_PARAMS];
  int paramCount = 0;
  if (parser->types != NULL && !parseTypeParams(parser, params, &paramCount)) return NULL;

  return parseGenericClassBody(parser, line, name, nameLength, params, paramCount);
}

/* Everything after the name, which a class expression has none of. */
AstNode *parseClassBody(Parser *parser, int line, const char *name, int nameLength) {
  return parseGenericClassBody(parser, line, name, nameLength, NULL, 0);
}

AstNode *parseGenericClassBody(Parser *parser, int line, const char *name, int nameLength, const TypeId *params, int paramCount) {
  const char *superName = NULL;
  int superLength = 0;
  /* `class Derived extends Base<number>` — the arguments belong to the shape
   * the members are copied from, and to nothing else: what the class extends
   * at run time is the binding `Base`. TYPE_DYNAMIC when none were written,
   * and the base is then looked up by name below. */
  TypeId superType = TYPE_DYNAMIC;
  if (matchToken(parser, TOKEN_EXTENDS)) {
    if (startsGenericConstruction(parser)) {
      Token named = parser->current;
      if (!parseTypeExpression(parser, &superType)) return NULL;
      superName = named.start;
      superLength = named.length;
    } else {
      consume(parser, TOKEN_IDENTIFIER, "expected a superclass name after 'extends'");
      if (parser->diag->panicMode) return NULL;
      superName = parser->previous.start;
      superLength = parser->previous.length;
    }
    if (superLength == nameLength && memcmp(superName, name, (size_t)superLength) == 0) {
      errorAtCurrent(parser, "a class cannot extend itself");
      return NULL;
    }
  }

  AstNode *node = csAstClass(parser->arena, line, name, nameLength, superName, superLength);

  /* A class's name is a type, and the shape behind it is built here as the
   * members are read — the parser is where a name in an annotation is
   * resolved, so this is the only place early enough to matter. It is
   * registered before the body, so a method may take or answer one of its own
   * class.
   *
   * A class expression has a name only for diagnostics and binds nothing, so
   * it declares no type either. */
  TypeId declared = TYPE_DYNAMIC;
  if (parser->types != NULL && !node->as.classDecl.isExpression && name != NULL && nameLength > 0 && name[0] != ' ') {
    TypeId taken;
    if (csTypeLookupName(parser->types, name, nameLength, &taken)) {
      csDiagnosticError(parser->diag, line, name, nameLength, "'%.*s' already names a type", nameLength, name);
      return NULL;
    }
    int ownedLength;
    const char *owned = csAstInternName(parser->arena, name, nameLength, &ownedLength);
    if (owned != NULL) declared = csTypeDeclareClass(parser->types, owned, ownedLength);
    if (declared == TYPE_ERROR) declared = TYPE_DYNAMIC;
    /* What `Box<number>` substitutes into. Recorded on the shape itself, which
     * is what makes a class's name a generic in exactly the way an interface's
     * is — there is no second kind. */
    if (declared != TYPE_DYNAMIC && paramCount > 0) csTypeSetTypeParams(parser->types, declared, params, paramCount);
  }

  /* `class Dog extends Animal` — what the base declared is part of this
   * shape too, copied across for the same reason `interface … extends` copies
   * it: assignability is structural, so presence is all that is ever asked. */
  if (declared != TYPE_DYNAMIC && superName != NULL) {
    TypeId base = superType;
    if (base != TYPE_DYNAMIC || csTypeLookupName(parser->types, superName, superLength, &base)) {
      const CompositeType *shape = csTypeComposite(parser->types, base);
      if (shape != NULL && shape->kind == COMPOSITE_INTERFACE) {
        int start = shape->memberStart;
        int count = shape->memberCount;
        for (int i = 0; i < count; i++) {
          TypeMember inherited = parser->types->members[start + i];
          csTypeAddMember(parser->types, declared, &inherited);
        }
      }
    }
  }

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

      /* A method is a member holding a function type, so `dog.speak()` is
       * checked and typed by the same rule as any other call. A static one
       * belongs to the class rather than to an instance, a private one is
       * invisible from outside, and a computed name is not known here. */
      if (!isConstructor && !isStatic && declared != TYPE_DYNAMIC && computedKey == NULL && memberName[0] != '#' && memberKind != MEMBER_SETTER) {
        TypeMember member;
        memset(&member, 0, sizeof member);
        member.name = csAstInternName(parser->arena, memberName, memberLength, &member.length);
        member.optional = false;
        if (memberKind == MEMBER_GETTER) {
          /* `get label(): string` is read as a string, not called. */
          member.type = method->as.function.returnType;
        } else {
          TypeId taken[CS_MAX_TYPE_PARAMS * 8];
          int takenCount = method->as.function.paramCount;
          if (takenCount > (int)(sizeof taken / sizeof taken[0])) takenCount = (int)(sizeof taken / sizeof taken[0]);
          for (int i = 0; i < takenCount; i++) {
            taken[i] = method->as.function.params[i].hasAnnotation ? method->as.function.params[i].type : TYPE_DYNAMIC;
          }
          int required = takenCount;
          for (int i = 0; i < takenCount; i++) {
            if (method->as.function.params[i].defaultValue == NULL) continue;
            required = i;
            break;
          }
          if (method->as.function.hasRest && required > 0) required--;
          member.type = csTypeFunctionOf(parser->types, taken, takenCount, required, method->as.function.hasRest, method->as.function.returnType);
        }
        if (member.name != NULL) csTypeAddMember(parser->types, declared, &member);
      }

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

        /* What the constructor takes, as a function type on the shape itself.
         * It is what makes `new Box(3)` a `Box<number>`: the parameters are
         * written in terms of the class's own type variables, so matching the
         * arguments against them says what each one stands for — the same
         * inference a generic function call does. */
        if (declared != TYPE_DYNAMIC && parser->types != NULL) {
          TypeId taken[CS_MAX_TYPE_PARAMS * 8];
          int takenCount = method->as.function.paramCount;
          if (takenCount > (int)(sizeof taken / sizeof taken[0])) takenCount = (int)(sizeof taken / sizeof taken[0]);
          for (int i = 0; i < takenCount; i++) {
            taken[i] = method->as.function.params[i].hasAnnotation ? method->as.function.params[i].type : TYPE_DYNAMIC;
          }
          int required = takenCount;
          for (int i = 0; i < takenCount; i++) {
            if (method->as.function.params[i].defaultValue == NULL) continue;
            required = i;
            break;
          }
          if (method->as.function.hasRest && required > 0) required--;
          csTypeSetConstructor(parser->types, declared, csTypeFunctionOf(parser->types, taken, takenCount, required, method->as.function.hasRest, TYPE_UNDEFINED));
        }
      } else {
        csAstClassAddMember(parser->arena, node, method, isStatic, memberKind);
        node->as.classDecl.members[node->as.classDecl.memberCount - 1].computedKey = computedKey;
      }
      continue;
    }

    TypeId fieldType;
    bool annotated;
    if (!parseTypeAnnotation(parser, &fieldType, &annotated)) return NULL;

    AstNode *initializer = NULL;
    if (matchToken(parser, TOKEN_EQUAL)) {
      initializer = parseExpression(parser);
      if (initializer == NULL) return NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the field declaration");
    if (parser->diag->panicMode) return NULL;

    /* A field is a member of the shape when it is written down here. One the
     * constructor adds instead is why a class's shape is open. */
    if (!isStatic && declared != TYPE_DYNAMIC && computedKey == NULL && memberName[0] != '#') {
      TypeMember member;
      memset(&member, 0, sizeof member);
      member.name = csAstInternName(parser->arena, memberName, memberLength, &member.length);
      member.length = memberLength;
      member.type = annotated ? fieldType : TYPE_DYNAMIC;
      member.optional = false;
      if (member.name != NULL) csTypeAddMember(parser->types, declared, &member);
    }

    csAstClassAddField(parser->arena, node, memberName, memberLength, initializer, fieldType, annotated, isStatic);
    node->as.classDecl.fields[node->as.classDecl.fieldCount - 1].computedKey = computedKey;
  }

  consume(parser, TOKEN_RIGHT_BRACE, "expected '}' after the class body");
  /* The parameters go out of scope with the declaration, so a later `T` means
   * whatever it means there. Closed even on the error path, because a name
   * left open would resolve in the rest of the file. */
  if (paramCount > 0) closeTypeParams(parser, params, paramCount);
  if (parser->diag->panicMode) return NULL;
  return node;
}

/* The `{ a, b as c }` shared by import and export lists. */
