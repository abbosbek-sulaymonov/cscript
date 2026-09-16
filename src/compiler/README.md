# `src/compiler/` — source text to bytecode

Four passes, one pass each, in this order:

```
text ──▶ lexer ──▶ parser ──▶ type checker ──▶ code generator ──▶ Chunk
          tokens     AST      annotated AST        bytecode
```

The checker sits between parsing and code generation because it needs the
whole tree, and because the generator *uses* what it resolves: a `+` between
two values proved numeric compiles to `OP_ADD_NUM` rather than `OP_ADD`. An
annotation is consumed, not erased.

## The files

| Lexing | Holds |
| --- | --- |
| [`lexer.c`](lexer.c) | Source text to tokens, one at a time, with no buffering |
| [`lexer_keyword.c`](lexer_keyword.c) | The keyword trie, and every token's printable name |
| [`lexer_internal.h`](lexer_internal.h) | The seam between those two |

| Parsing | Holds |
| --- | --- |
| [`parser.c`](parser.c) | Token plumbing, precedence and operator tables, literal decoding, `csParse` |
| [`parser_expression.c`](parser_expression.c) | Precedence climbing, and the primary dispatcher |
| [`parser_primary.c`](parser_primary.c) | The operands an expression can start with |
| [`parser_prefix.c`](parser_prefix.c) | The operators an expression can start with |
| [`parser_arrow.c`](parser_arrow.c) | Template literals, arrows, and the function forms |
| [`parser_statement.c`](parser_statement.c) | Blocks, conditionals, the four loop forms, `switch`, `try` |
| [`parser_type.c`](parser_type.c) | `interface` and `type` — the two declarations that describe rather than produce |
| [`parser_declaration.c`](parser_declaration.c) | `let`, `const`, and the patterns either can destructure into |
| [`parser_class.c`](parser_class.c) | The class body, in source order |
| [`parser_module.c`](parser_module.c) | `import` and `export`, in every form |
| [`parser_internal.h`](parser_internal.h) | What all of those share |

| The tree | Holds |
| --- | --- |
| [`ast.c`](ast.c) | The arena, and the expression nodes |
| [`ast_statement.c`](ast_statement.c) | The statement and declaration nodes, and the lists |
| [`ast_name.c`](ast_name.c) | An operator's name, for diagnostics and the dump |
| [`ast_internal.h`](ast_internal.h) | How a node and a list are made |

| Checking | Holds |
| --- | --- |
| [`typecheck.c`](typecheck.c) | The scope, the builtins, the signatures, the node dispatcher |
| [`typecheck_value.c`](typecheck_value.c) | The type of an expression |
| [`typecheck_statement.c`](typecheck_statement.c) | The checking a statement needs — which is mostly the scope |
| [`typecheck_internal.h`](typecheck_internal.h) | The checker's state |
| [`type.c`](type.c) | The type names, and what may be assigned to what |
| [`type_table.c`](type_table.c) | The table the composite types live in — arrays, functions, unions and shapes |
| [`type_generic.c`](type_generic.c) | Substitution, instantiation, and what a call site infers |
| [`type_prelude.c`](type_prelude.c) | The generic types every file starts with: `Map`, `Set`, `Promise` |
| [`type_import.c`](type_import.c) | Re-interning a type from one file's table into another's |
| [`typecheck_module.c`](typecheck_module.c) | The types that cross a file boundary |
| [`typecheck_narrow.c`](typecheck_narrow.c) | What a `typeof`, a null check or a guard proves about the thing it tested |
| [`typecheck_expect.c`](typecheck_expect.c) | The two checks that need to know what was expected: a literal against a shape, a call through a function type |

| Code generation | Holds |
| --- | --- |
| [`compiler.c`](compiler.c) | Emit helpers, scopes, locals, upvalue capture, the node dispatcher |
| [`compiler_node_value.c`](compiler_node_value.c) | What each expression node becomes |
| [`compiler_node_statement.c`](compiler_node_statement.c) | What each statement node becomes |
| [`compiler_expression.c`](compiler_expression.c) | Operators, and the conditions that feed a jump |
| [`compiler_assign.c`](compiler_assign.c) | Assignment, in all the forms it takes |
| [`compiler_function.c`](compiler_function.c) | Functions, and what `this` means inside one |
| [`compiler_statement.c`](compiler_statement.c) | Control flow, and the destructuring a declaration lowers to |
| [`compiler_class.c`](compiler_class.c) | Classes: members, accessors, statics, constructors |
| [`compiler_module.c`](compiler_module.c) | Imports and exports, resolved at compile time |
| [`compiler_internal.h`](compiler_internal.h) | The compiler's ambient state, and the seams |
| [`chunk.c`](chunk.c) | The bytecode buffer, the constant pool, the inline-cache arrays |

| Looking at the result | Holds |
| --- | --- |
| [`debug.c`](debug.c) | The disassembler, the dump flags, and `csInstructionLength` |
| [`debug_ast.c`](debug_ast.c) | The parse tree, one line per node, with the resolved type beside it |
| [`diagnostic.c`](diagnostic.c) | One message per error, with the source line quoted under it |

## Three things worth knowing before changing any of it

**How a parser group says "not mine".** `parsePrimary` asks
[`parser_primary.c`](parser_primary.c) and then
[`parser_prefix.c`](parser_prefix.c), and each answers `NULL` for a token it
does not own. `NULL` alone is ambiguous — it also means "mine, and malformed"
— so what separates the two is **whether the group consumed a token**, not the
error count: a diagnostic is suppressed while the parser is recovering, so a
group can fail without the count moving. Getting that wrong made
`console.log(1 +); console.log(2 * );` report the second line's error as the
wrong thing, which is what [`parser_expression.c`](parser_expression.c) records
above `parsePrimary`.

**The code generator dispatches twice.** [`compiler.c`](compiler.c) hands a
node to `compiler_node_value.c` and then to `compiler_node_statement.c`, each
of which answers `false` for a node it does not handle; the larger forms are
one line there handing off to the file that owns them. The contract those files
maintain is the stack discipline — an expression leaves exactly one value, a
statement leaves the stack as it found it — and a generator that emits the
wrong number is a bug the disassembler will not show you.

**`csInstructionLength` is the only thing that knows how long an instruction
is.** It lives in [`debug.c`](debug.c) beside the disassembler because they
walk the same operand layout, and both the tiering scan and the JIT's lowering
stride a chunk with it. A second copy that drifts reads an operand as an
opcode.

## Related

- [../../docs/GRAMMAR.md](../../docs/GRAMMAR.md) — the syntax as EBNF, and what is not implemented
- [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md#the-type-checker) — why the checker is where it is, and what the types buy the code generator
- `--print-tokens`, `--print-ast`, `--print-bytecode` — each pass's output, from the CLI ([CLI.md](../../docs/CLI.md))
