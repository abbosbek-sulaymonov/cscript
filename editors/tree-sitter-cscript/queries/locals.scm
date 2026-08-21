; Scopes and definitions, for rename and go-to-definition in editors that use
; tree-sitter for them.

(statement_block) @local.scope
(function_declaration) @local.scope
(function_expression) @local.scope
(arrow_function) @local.scope
(method_definition) @local.scope
(class_declaration) @local.scope
(for_statement) @local.scope
(for_of_statement) @local.scope
(catch_clause) @local.scope

(variable_declarator name: (identifier) @local.definition.var)
(parameter name: (identifier) @local.definition.parameter)
(function_declaration name: (identifier) @local.definition.function)
(class_declaration name: (identifier) @local.definition.type)
(import_specifier name: (identifier) @local.definition.import)
(import_specifier alias: (identifier) @local.definition.import)
(namespace_import (identifier) @local.definition.import)
(catch_clause parameter: (identifier) @local.definition.var)
(rest_pattern (identifier) @local.definition.var)

(identifier) @local.reference
