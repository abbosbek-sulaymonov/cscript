/* parser_expression.c — an expression, by precedence climbing.
 *
 * Parse a primary, then keep folding in operators whose precedence is at least
 * the caller's. Binary operators are left-associative, so their right side is
 * parsed one level tighter; assignment is right-associative and parses at its
 * own level. What a primary can be is in parser_primary.c and
 * parser_prefix.c.
 */
#include <stdio.h>
#include <string.h>

#include "cscript/parser.h"
#include "compiler/parser_internal.h"


/* The argument list of a call, after its '(' has been consumed. Shared by the
 * plain and optional forms so the two cannot drift apart. */
static bool parseCallArguments(Parser *parser, AstNode *call, int line) {
  if (!check(parser, TOKEN_RIGHT_PAREN)) {
    do {
      bool isSpread = matchToken(parser, TOKEN_ELLIPSIS);
      AstNode *argument = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (argument == NULL) return false;
      if (isSpread) argument = csAstSpread(parser->arena, line, argument);
      csAstCallAddArgument(parser->arena, call, argument);
    } while (matchToken(parser, TOKEN_COMMA));
  }
  consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after arguments");
  return !parser->diag->panicMode;
}

/* True when a logical operator is being written next to `?\?` without
 * parentheses, which JavaScript rejects outright. */
static bool rejectMixedNullish(Parser *parser, const AstNode *left,
                               TokenType operatorType, int line) {
  bool joiningNullish = operatorType == TOKEN_QUESTION_QUESTION;
  bool joiningLogical =
      operatorType == TOKEN_AMP_AMP || operatorType == TOKEN_PIPE_PIPE;
  if (!joiningNullish && !joiningLogical) return false;
  if (left == NULL || left->type != AST_LOGICAL) return false;

  bool leftIsNullish = left->as.logical.op == LOGICAL_NULLISH;
  if (joiningNullish == leftIsNullish) return false;

  csDiagnosticError(parser->diag, line, NULL, 0,
                    "'?\?' cannot be mixed with '&&' or '||' without "
                    "parentheses saying which was meant");
  return true;
}

/* Postfix `.name` and `(args)`, which bind tighter than any unary operator. */
AstNode *parseCallSuffixes(Parser *parser, AstNode *expression) {
  /* Set by the first `?.`. See csAstOptionalChain: the links short-circuit
   * the whole chain, so the chain has to exist as a node. */
  bool sawOptional = false;

  for (;;) {
    if (check(parser, TOKEN_QUESTION_DOT)) {
      int line = parser->current.line;
      advanceToken(parser);
      sawOptional = true;

      if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
        AstNode *index = parseExpression(parser);
        consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the index");
        if (index == NULL || parser->diag->panicMode) return NULL;
        expression = csAstIndex(parser->arena, line, expression, index);
        expression->as.index.optional = true;
        continue;
      }

      if (check(parser, TOKEN_LEFT_PAREN)) {
        advanceToken(parser);
        AstNode *call = csAstCall(parser->arena, line, expression);
        if (!parseCallArguments(parser, call, line)) return NULL;
        call->as.call.optional = true;
        expression = call;
        continue;
      }

      if (!consumePropertyName(parser, "expected a property name after '?.'")) return NULL;
      if (parser->diag->panicMode) return NULL;
      expression = csAstProperty(parser->arena, line, expression,
                                 parser->previous.start, parser->previous.length);
      expression->as.property.optional = true;
      continue;
    }

    if (matchToken(parser, TOKEN_DOT)) {
      int line = parser->previous.line;
      if (!consumePropertyName(parser, "expected a property name after '.'")) return NULL;
      if (parser->diag->panicMode) return NULL;
      expression = csAstProperty(parser->arena, line, expression,
                                 parser->previous.start, parser->previous.length);
      continue;
    }

    if (matchToken(parser, TOKEN_LEFT_BRACKET)) {
      int line = parser->previous.line;
      AstNode *index = parseExpression(parser);
      consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the index");
      if (index == NULL || parser->diag->panicMode) return NULL;
      expression = csAstIndex(parser->arena, line, expression, index);
      continue;
    }

    if (matchToken(parser, TOKEN_LEFT_PAREN)) {
      int line = parser->previous.line;
      AstNode *call = csAstCall(parser->arena, line, expression);
      if (!parseCallArguments(parser, call, line)) return NULL;
      expression = call;
      continue;
    }

    /* `` tag`…` `` — a template right after an expression is a call, with the
     * literal's pieces as the first argument. */
    if (check(parser, TOKEN_TEMPLATE)) {
      advanceToken(parser);
      expression = parseTaggedTemplate(parser, expression, parser->previous.start,
                                       parser->previous.length,
                                       parser->previous.line);
      if (expression == NULL) return NULL;
      continue;
    }

    /* Postfix ++/--. It binds tighter than any unary operator, and yields the
     * value from *before* the update, which is why it needs its own node
     * rather than desugaring to an assignment. */
    if (check(parser, TOKEN_PLUS_PLUS) || check(parser, TOKEN_MINUS_MINUS)) {
      bool isIncrement = check(parser, TOKEN_PLUS_PLUS);
      int line = parser->current.line;
      if (expression->type != AST_IDENTIFIER) {
        errorAtCurrent(parser, "'++' and '--' need a variable to update");
        return NULL;
      }
      advanceToken(parser);
      expression = csAstUpdate(parser->arena, line, expression, isIncrement, false);
      continue;
    }

    /* The chain is complete, so this is where a nullish link lands. */
    if (sawOptional) {
      expression = csAstOptionalChain(parser->arena, expression->line, expression);
    }
    return expression;
  }
}

/* An expression's first token, sent to whichever group claims it.
 *
 * Two groups rather than one chain of thirty `if`s: the operands, and the
 * operators an expression can *begin* with. Asked in that order because an
 * operand is the common case, and what happens when neither claims the token
 * is the one thing this function still does itself.
 *
 * A group answers NULL both for a token it did not want and for one it did
 * want and could not parse, so the two have to be told apart — and what tells
 * them apart is whether it consumed anything. Not the error count: a
 * diagnostic is suppressed while the parser is recovering, so a group can fail
 * without the count moving, and `console.log(1 +); console.log(2 * );` reported
 * the second line's error as the wrong thing when that was the test. */
AstNode *parsePrimary(Parser *parser) {
  int line = parser->current.line;
  const char *unconsumed = parser->current.start;

  AstNode *operand = parseOperandPrimary(parser, line);
  if (operand != NULL) return operand;
  if (parser->current.start != unconsumed) return NULL;

  AstNode *prefixed = parsePrefixPrimary(parser, line);
  if (prefixed != NULL) return prefixed;
  if (parser->current.start != unconsumed) return NULL;

  if (rejectLooseEquality(parser)) return NULL;

  errorAtCurrent(parser, "expected an expression");
  return NULL;
}


/* Precedence climbing: parse a primary, then keep folding in operators whose
 * precedence is at least `minPrecedence`. Binary operators here are
 * left-associative, so their right side is parsed one level tighter;
 * assignment is right-associative and so parses at its own level. */
AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence) {
  AstNode *left = parsePrimary(parser);
  if (left == NULL) return NULL;

  for (;;) {
    /* Assignment, handled before the table because it is right-associative and
     * needs the left side to be a valid target rather than a value. */
    BinaryOp compound = BINARY_ADD;
    AssignKind logical;
    bool isPlain = check(parser, TOKEN_EQUAL);
    bool isCompound = compoundAssignOp(parser->current.type, &compound);
    bool isLogical = logicalAssignKind(parser->current.type, &logical);

    if ((isPlain || isCompound || isLogical) && minPrecedence <= PREC_ASSIGNMENT) {
      int line = parser->current.line;
      advanceToken(parser);

      if (left->type != AST_IDENTIFIER && left->type != AST_PROPERTY &&
          left->type != AST_INDEX) {
        csDiagnosticError(parser->diag, line, NULL, 0,
                          "the left side of an assignment must be a variable, "
                          "a property or an index");
        return NULL;
      }

      AstNode *value = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (value == NULL) return NULL;

      /* The kind travels on the node rather than being expanded into
       * `target = target op value` here. Expanding it would compile the
       * target twice, so `f().x += 1` would call `f` twice — which it did,
       * until this stopped being a desugaring. */
      AssignKind kind = isPlain ? ASSIGN_PLAIN : isCompound ? ASSIGN_COMPOUND : logical;
      left = csAstAssignKind(parser->arena, line, left, value, kind, compound);
      continue;
    }

    if (check(parser, TOKEN_COMMA) && minPrecedence <= PREC_COMMA) {
      int line = parser->current.line;
      advanceToken(parser);
      AstNode *second = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (second == NULL) return NULL;
      left = csAstSequence(parser->arena, line, left, second);
      continue;
    }

    /* `?:` is right-associative and binds looser than everything except
     * assignment, so both arms parse at the conditional level. */
    if (check(parser, TOKEN_QUESTION) && minPrecedence <= PREC_CONDITIONAL) {
      int line = parser->current.line;
      advanceToken(parser);

      AstNode *thenValue = parsePrecedence(parser, PREC_ASSIGNMENT);
      if (thenValue == NULL) return NULL;
      consume(parser, TOKEN_COLON, "expected ':' in a conditional expression");
      if (parser->diag->panicMode) return NULL;
      AstNode *elseValue = parsePrecedence(parser, PREC_CONDITIONAL);
      if (elseValue == NULL) return NULL;

      left = csAstConditional(parser->arena, line, left, thenValue, elseValue);
      continue;
    }

    if (rejectLooseEquality(parser)) return NULL;

    Precedence precedence = binaryPrecedence(parser->current.type);
    if (precedence == PREC_NONE || precedence < minPrecedence) break;

    TokenType operatorType = parser->current.type;
    int line = parser->current.line;

    /* `a || b ?? c` is a syntax error in JavaScript, not a precedence
     * question. The two operators disagree about what counts as "no value",
     * so any grouping the language picked would be a coin-flip for whoever
     * reads it next; requiring the parentheses says which was meant. */
    if (rejectMixedNullish(parser, left, operatorType, line)) return NULL;

    advanceToken(parser);

    /* ** is the one right-associative binary operator, so 2 ** 3 ** 2 groups
     * as 2 ** (3 ** 2). Every other operator parses its right side one level
     * tighter, which is what makes them left-associative. */
    Precedence rightPrecedence =
        operatorType == TOKEN_STAR_STAR ? precedence : (Precedence)(precedence + 1);
    AstNode *right = parsePrecedence(parser, rightPrecedence);
    if (right == NULL) return NULL;

    if (operatorType == TOKEN_AMP_AMP) {
      left = csAstLogical(parser->arena, line, LOGICAL_AND, left, right);
    } else if (operatorType == TOKEN_PIPE_PIPE) {
      left = csAstLogical(parser->arena, line, LOGICAL_OR, left, right);
    } else if (operatorType == TOKEN_QUESTION_QUESTION) {
      left = csAstLogical(parser->arena, line, LOGICAL_NULLISH, left, right);
    } else {
      left = csAstBinary(parser->arena, line, binaryOpFor(operatorType), left, right);
    }
    if (left == NULL) return NULL;
  }

  return left;
}

AstNode *parseExpression(Parser *parser) {
  /* The comma *operator*, which only exists where a whole expression is
   * wanted. Everywhere a comma separates things — arguments, array elements,
   * object entries, declarators — the parser asks for an assignment instead,
   * so a separator can never be mistaken for one. */
  return parsePrecedence(parser, PREC_COMMA);
}

/* Builds the concatenation a template literal desugars to.
 *
 * `\`a ${x} b\`` becomes `"a" + x + " b"`, which reuses the existing string
 * semantics of `+` — including the rule that a string on either side wins — so
 * interpolating a number needs no extra machinery.
 *
 * The whole literal arrived as one token, so each `${...}` is re-lexed here by
 * a nested parser over a NUL-terminated copy. Its diagnostics share this
 * parser's, and line numbers are reported relative to the template's own line,
 * which is exact unless the template spans lines. */
