; Highlighting for CScript. The scope names follow tree-sitter's conventions
; rather than the TextMate grammar's, but the two make the same distinctions —
; a BigInt is not a number, a type annotation is not an identifier.

(comment) @comment

; The two things that are not JavaScript.
(primitive_type) @type.builtin
(bigint) @number

(number) @number
(string) @string
(escape_sequence) @string.escape
(template_string) @string
(regex_pattern) @string.regex
(regex_flags) @string.escape

[
  "if" "else" "switch" "case" "default"
  "for" "while" "do" "of" "in"
  "return" "break" "continue" "throw"
  "try" "catch" "finally"
] @keyword

["import" "export" "from" "as"] @keyword.import
["function" "class" "let" "const"] @keyword
["async" "static" "extends" "get" "set"] @keyword.modifier
["await" "yield"] @keyword.coroutine
["typeof" "instanceof" "delete" "void" "new"] @keyword.operator

[(true) (false)] @boolean
[(null) (undefined)] @constant.builtin
(this) @variable.builtin
(super) @variable.builtin
(new_target) @variable.builtin

(function_declaration name: (identifier) @function)
(method_definition name: (identifier) @function.method)
(object_method name: (identifier) @function.method)
; A call's callee is an expression, so the identifier sits one node down.
(call_expression
  function: (primary_expression (identifier) @function.call))
(call_expression
  function: (primary_expression
    (member_expression property: (identifier) @function.method.call)))

(class_declaration name: (identifier) @type)
(class_declaration superclass: (identifier) @type)
(class_expression name: (identifier) @type)

(private_property_identifier) @property
(member_expression property: (identifier) @property)
(pair key: (identifier) @property)
(field_definition name: (identifier) @property)

(labeled_statement label: (identifier) @label)
(break_statement label: (identifier) @label)
(continue_statement label: (identifier) @label)

(parameter name: (identifier) @variable.parameter)
(identifier) @variable

; The standard library is part of the language here — frozen, and not
; reassignable — so it reads as built-in rather than as any other name.
((identifier) @module.builtin
  (#any-of? @module.builtin
    "console" "Math" "JSON" "Object" "Array" "Number" "String" "Boolean"
    "Symbol" "BigInt" "Promise" "Map" "Set" "WeakMap" "WeakSet" "Date"
    "Error" "AggregateError" "RegExp"))

((identifier) @function.builtin
  (#any-of? @function.builtin
    "parseInt" "parseFloat" "isNaN" "isFinite"
    "setTimeout" "setInterval" "clearTimeout" "clearInterval"))

[
  "=" "+=" "-=" "*=" "/=" "%=" "**=" "&&=" "||=" "??="
  "===" "!==" "<" "<=" ">" ">=" "+" "-" "*" "/" "%" "**"
  "&&" "||" "??" "!" "++" "--" "=>" "..." "?." "?" ":"
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["." "," ";"] @punctuation.delimiter
["${" "}"] @punctuation.special
