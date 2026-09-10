/* lexer_internal.h — the seam between the lexer's two files.
 *
 * The scanner and the keyword table. Nothing outside src/compiler/lexer*.c
 * includes this.
 */
#ifndef CSCRIPT_COMPILER_LEXER_INTERNAL_H
#define CSCRIPT_COMPILER_LEXER_INTERNAL_H

#include "cscript/lexer.h"

/* Whether the identifier the lexer has just scanned is a keyword, and which.
 * TOKEN_IDENTIFIER when it is not. */
TokenType csLexerIdentifierType(const Lexer *lexer);

#endif /* CSCRIPT_COMPILER_LEXER_INTERNAL_H */
