/* parser_internal.h — what the parser's four translation units share.
 *
 * The parser is one recursive-descent pass split by *what it parses* rather
 * than by phase: expressions, declarations, statements, and the token
 * plumbing they all sit on. The split is for reading, not for layering — the
 * pieces are mutually recursive, because the grammar is (a statement holds an
 * expression, an expression may hold a function body full of statements), and
 * this header is what lets them call each other.
 *
 * Nothing outside the parser includes this. The public entry point is csParse
 * in parser.h.
 */
#ifndef CSCRIPT_PARSER_INTERNAL_H
#define CSCRIPT_PARSER_INTERNAL_H

#include "cscript/ast.h"
#include "cscript/parser.h"

/* Binding powers, lowest binds loosest. Mirrors JS operator precedence for the
 * operators that exist today; new levels slot in without touching the parser
 * functions, because binary parsing is driven by this table. */
typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  /* = += -= *= /= %=  (right-associative) */
  PREC_CONDITIONAL, /* ?:                (right-associative) */
  PREC_OR,         /* ||               */
  PREC_AND,        /* &&               */
  PREC_EQUALITY,   /* === !==          */
  PREC_COMPARISON, /* < > <= >=        */
  PREC_TERM,       /* + -              */
  PREC_FACTOR,     /* * / %            */
  PREC_EXPONENT,   /* **  (right-assoc)*/
  PREC_UNARY,      /* ! - typeof ++ -- */
  PREC_CALL,       /* . ( )            */
} Precedence;


/* parser.c — token plumbing and the shared tables, used by all four units */
void advanceToken(Parser *parser);
bool check(const Parser *parser, TokenType type);
bool matchToken(Parser *parser, TokenType type);
void errorAtCurrent(Parser *parser, const char *message);
void consume(Parser *parser, TokenType type, const char *message);
void synchronize(Parser *parser);
Precedence binaryPrecedence(TokenType type);
BinaryOp binaryOpFor(TokenType type);
AstNode *makeStringLiteral(Parser *parser, const char *start, int length,
                                  int line);
double parseNumberLiteral(const char *start, int length);
bool rejectLooseEquality(Parser *parser);
bool rejectInOperator(Parser *parser);
bool consumePropertyName(Parser *parser, const char *message);
bool nameIsWord(const char *name, int length, const char *word);
bool checkWord(Parser *parser, const char *word);
bool nextStartsFunction(Parser *parser);
bool nextStartsArrowParams(Parser *parser);
bool compoundAssignOp(TokenType type, BinaryOp *out);
bool parseTypeAnnotation(Parser *parser, TypeKind *type, bool *present);
bool nameIs(const char *name, int length, const char *word);
bool checkContextual(Parser *parser, const char *word);
bool matchContextual(Parser *parser, const char *word);
/* src/parser_expression.c */
AstNode *parseCallSuffixes(Parser *parser, AstNode *expression);
AstNode *parsePrimary(Parser *parser);
AstNode *parsePrecedence(Parser *parser, Precedence minPrecedence);
AstNode *parseExpression(Parser *parser);
AstNode *parseTemplate(Parser *parser, const char *start, int length, int line);
bool looksLikeArrowParams(Parser *parser);
AstNode *finishArrow(Parser *parser, AstNode *function, int line);
AstNode *parseFunctionRest(Parser *parser, int line, const char *name,
                                  int nameLength, bool isMethod);
AstNode *parseFunction(Parser *parser, bool requireName);

/* src/parser_declaration.c */
AstNode *parseClass(Parser *parser);
bool parseModuleNameList(Parser *parser, AstNode *node, bool isImport);
bool parseModuleSpecifier(Parser *parser, const char **out, int *outLength);
AstNode *parseImport(Parser *parser);
AstNode *parseExport(Parser *parser);
AstNode *parsePattern(Parser *parser, bool isObject, bool isConst);
AstNode *parseDestructuring(Parser *parser, bool isObject, bool isConst);
AstNode *finishVarDeclaration(Parser *parser, int line, const char *name,
                                     int nameLength, bool isConst);
AstNode *parseVarDeclaration(Parser *parser, bool isConst);

/* src/parser_statement.c */
AstNode *parseTry(Parser *parser);
AstNode *parseSwitch(Parser *parser);
AstNode *parseBlock(Parser *parser);
AstNode *parseIf(Parser *parser);
AstNode *parseDoWhile(Parser *parser);
AstNode *parseWhile(Parser *parser);
AstNode *parseFor(Parser *parser);
AstNode *parseStatement(Parser *parser);
const char *notImplementedMessage(TokenType type);

#endif /* CSCRIPT_PARSER_INTERNAL_H */
