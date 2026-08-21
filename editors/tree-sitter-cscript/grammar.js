/**
 * CScript, for tree-sitter.
 *
 * Transcribed from docs/GRAMMAR.md in the compiler repository, which is what
 * the parser implements — the EBNF there is the source of truth, and a rule
 * that disagrees with it is a bug in this file rather than a dialect.
 *
 * Where CScript differs from JavaScript, this differs too, and deliberately:
 * there is no `var`, no `==`, no automatic semicolon insertion, and a
 * declaration may carry a type annotation. Leaving those in "because
 * JavaScript has them" would make the editor accept what the compiler refuses,
 * which is the failure this grammar exists to avoid.
 */

// Loosest binding first, matching the precedence table in docs/GRAMMAR.md.
const PREC = {
  sequence: 0,
  assignment: 1,
  ternary: 2,
  or: 3,
  and: 4,
  equality: 5,
  relational: 6,
  additive: 7,
  multiplicative: 8,
  exponent: 9,
  unary: 10,
  postfix: 11,
  call: 12,
  member: 13,
};

const TYPE_NAMES = [
  "number", "bigint", "string", "boolean", "null", "undefined", "object", "any",
];

module.exports = grammar({
  name: "cscript",

  extras: ($) => [$.comment, /[\s﻿⁠​]/],

  // A template literal holds expressions, and an expression may hold another
  // template; the conflict is genuine and resolved by the parser rather than
  // by flattening one of them.
  conflicts: ($) => [
    [$.primary_expression, $._property_name],
    [$.object, $.object_pattern],
    [$.array, $.array_pattern],
    // `{` at the start of a statement is a block, never an object literal.
    // JavaScript draws the same line, and for the same reason: the two are
    // indistinguishable until something inside them settles it.
    [$.statement_block, $.object],
    // `(a = 7, b = 8)` is a parameter list until the `=>` says so, and a
    // parenthesised expression otherwise. Neither can be decided at the open
    // paren, which is what makes this a genuine conflict rather than a
    // precedence.
    [$.arrow_function, $.primary_expression],
    [$.formal_parameters, $.parenthesized_expression],
    [$.parameter, $.assignment_expression],
    [$.rest_pattern, $.primary_expression],
    [$.function_declaration, $.function_expression],
    [$._expression, $.sequence_expression],
    [$.call_expression, $.new_expression],
    [$._pattern_element, $.primary_expression],
    [$._property_name, $._object_pattern_entry],
    [$._object_pattern_entry, $.shorthand_property],
    [$.class_declaration, $.class_expression],
    [$.parameter, $.primary_expression],
    [$.primary_expression, $.shorthand_property],
  ],

  word: ($) => $.identifier,

  rules: {
    program: ($) => repeat($._statement),

    // ---- statements ------------------------------------------------------

    _statement: ($) =>
      choice(
        $.variable_declaration,
        $.function_declaration,
        $.class_declaration,
        $.import_statement,
        $.export_statement,
        $.statement_block,
        $.if_statement,
        $.while_statement,
        $.do_statement,
        $.for_statement,
        $.for_of_statement,
        $.switch_statement,
        $.try_statement,
        $.labeled_statement,
        $.break_statement,
        $.continue_statement,
        $.return_statement,
        $.throw_statement,
        $.empty_statement,
        $.expression_statement,
      ),

    // `let` and `const` only. There is no `var`: a binding that is visible
    // before its declaration is a bug the language does not offer.
    variable_declaration: ($) =>
      seq(
        field("kind", choice("let", "const")),
        commaSep1($.variable_declarator),
        ";",
      ),

    variable_declarator: ($) =>
      seq(
        field("name", choice($.identifier, $._pattern)),
        optional($.type_annotation),
        optional(seq("=", field("value", $._single_expression))),
      ),

    // The one piece of syntax that is not JavaScript.
    type_annotation: ($) => seq(":", field("type", $.primitive_type)),
    primitive_type: () => choice(...TYPE_NAMES),

    function_declaration: ($) =>
      seq(
        optional(field("async", "async")),
        "function",
        optional(field("generator", "*")),
        field("name", $.identifier),
        field("parameters", $.formal_parameters),
        optional($.type_annotation),
        field("body", $.statement_block),
      ),

    class_declaration: ($) =>
      seq(
        "class",
        field("name", $.identifier),
        optional(seq("extends", field("superclass", $.identifier))),
        field("body", $.class_body),
      ),

    class_body: ($) => seq("{", repeat($._class_member), "}"),

    _class_member: ($) =>
      choice($.static_block, $.method_definition, $.field_definition),

    static_block: ($) => seq("static", $.statement_block),

    method_definition: ($) =>
      seq(
        optional("static"),
        optional(field("accessor", choice("get", "set"))),
        optional(field("async", "async")),
        optional(field("generator", "*")),
        field("name", $._property_name),
        field("parameters", $.formal_parameters),
        optional($.type_annotation),
        field("body", $.statement_block),
      ),

    field_definition: ($) =>
      seq(
        optional("static"),
        field("name", $._property_name),
        optional($.type_annotation),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    // A property may be named after a contextual keyword. `get`, `of` and the
    // rest are only keywords where the grammar is looking for one — `{ get: 1 }`
    // is an ordinary property and `{ get() {} }` an ordinary method, which is
    // the same rule the parser follows. Without this they are extracted as
    // keyword tokens and stop being usable as names at all.
    _property_name: ($) =>
      choice($.identifier, $.contextual_keyword, $.private_property_identifier,
             $.string, $.number, $.computed_property_name),

    contextual_keyword: () =>
      choice("get", "set", "of", "in", "as", "from", "static", "async",
             "target", "await", "yield"),
    // A single expression rather than a full one: a comma between brackets is
    // the enclosing list's separator far more often than it is the comma
    // operator, and `[(a, b)]` is how you say you meant the latter.
    computed_property_name: ($) => prec(1, seq("[", $._single_expression, "]")),

    formal_parameters: ($) => seq("(", commaSep($.parameter), ")"),
    parameter: ($) =>
      seq(
        optional(field("rest", "...")),
        field("name", choice($.identifier, $._pattern)),
        optional($.type_annotation),
        optional(seq("=", field("default", $._single_expression))),
      ),

    import_statement: ($) =>
      seq("import", $._import_clause, "from", field("source", $.string), ";"),
    _import_clause: ($) =>
      choice(
        $.namespace_import,
        $.named_imports,
        seq($.identifier, optional(seq(",", choice($.namespace_import, $.named_imports)))),
      ),
    namespace_import: ($) => seq("*", "as", $.identifier),
    named_imports: ($) => seq("{", commaSep($.import_specifier), "}"),
    import_specifier: ($) =>
      seq(field("name", $.identifier), optional(seq("as", field("alias", $.identifier)))),

    export_statement: ($) =>
      seq(
        "export",
        choice(
          $.variable_declaration,
          $.function_declaration,
          $.class_declaration,
          seq($.named_imports, optional(seq("from", field("source", $.string))), ";"),
          seq("*", "from", field("source", $.string), ";"),
          seq("default", choice($.function_declaration, $.class_declaration,
                                seq($._expression, ";"))),
        ),
      ),

    statement_block: ($) => prec.dynamic(1, seq("{", repeat($._statement), "}")),
    empty_statement: () => ";",
    expression_statement: ($) => seq($._expression, ";"),

    if_statement: ($) =>
      prec.right(
        seq("if", "(", field("condition", $._expression), ")",
            field("consequence", $._statement),
            optional(seq("else", field("alternative", $._statement)))),
      ),
    while_statement: ($) =>
      seq("while", "(", field("condition", $._expression), ")", field("body", $._statement)),
    do_statement: ($) =>
      seq("do", field("body", $._statement), "while", "(",
          field("condition", $._expression), ")", ";"),
    for_statement: ($) =>
      seq("for", "(",
          choice($.variable_declaration, $.expression_statement, ";"),
          optional(field("condition", $._expression)), ";",
          optional(field("increment", $._expression)), ")",
          field("body", $._statement)),
    for_of_statement: ($) =>
      seq("for", optional(field("await", "await")), "(",
          field("kind", choice("let", "const")),
          field("left", choice($.identifier, $._pattern)),
          field("operator", choice("of", "in")),
          field("right", $._expression), ")",
          field("body", $._statement)),

    switch_statement: ($) =>
      seq("switch", "(", field("subject", $._expression), ")", $.switch_body),
    switch_body: ($) => seq("{", repeat(choice($.switch_case, $.switch_default)), "}"),
    switch_case: ($) =>
      seq("case", field("value", $._expression), ":", repeat($._statement)),
    switch_default: ($) => seq("default", ":", repeat($._statement)),

    try_statement: ($) =>
      seq("try", field("body", $.statement_block),
          optional($.catch_clause), optional($.finally_clause)),
    catch_clause: ($) =>
      seq("catch", optional(seq("(", field("parameter", $.identifier), ")")),
          field("body", $.statement_block)),
    finally_clause: ($) => seq("finally", field("body", $.statement_block)),

    labeled_statement: ($) =>
      prec.dynamic(-1, seq(field("label", $.identifier), ":", $._statement)),
    break_statement: ($) => seq("break", optional(field("label", $.identifier)), ";"),
    continue_statement: ($) => seq("continue", optional(field("label", $.identifier)), ";"),
    return_statement: ($) => seq("return", optional($._expression), ";"),
    throw_statement: ($) => seq("throw", $._expression, ";"),

    // ---- patterns --------------------------------------------------------

    _pattern: ($) => choice($.array_pattern, $.object_pattern),
    array_pattern: ($) => seq("[", commaSep(optional($._pattern_element)), "]"),
    _pattern_element: ($) =>
      choice(seq(choice($.identifier, $._pattern), optional(seq("=", $._single_expression))),
             $.rest_pattern),
    object_pattern: ($) => seq("{", commaSep($._object_pattern_entry), "}"),
    _object_pattern_entry: ($) =>
      choice(
        seq(field("key", $.identifier),
            optional(seq(":", field("value", choice($.identifier, $._pattern)))),
            optional(seq("=", field("default", $._single_expression)))),
        $.rest_pattern,
      ),
    rest_pattern: ($) => seq("...", $.identifier),

    // ---- expressions -----------------------------------------------------

    _expression: ($) => choice($.sequence_expression, $._single_expression),
    sequence_expression: ($) =>
      prec.left(PREC.sequence, seq($._single_expression, ",", $._expression)),

    _single_expression: ($) =>
      choice(
        $.assignment_expression,
        $.ternary_expression,
        $.binary_expression,
        $.unary_expression,
        $.update_expression,
        $.yield_expression,
        $.arrow_function,
        $.function_expression,
        $.class_expression,
        $.primary_expression,
      ),

    assignment_expression: ($) =>
      prec.right(PREC.assignment,
        seq(field("left", choice($.identifier, $.member_expression, $.subscript_expression)),
            field("operator", choice("=", "+=", "-=", "*=", "/=", "%=", "**=",
                                     "&&=", "||=", "??=")),
            field("right", $._single_expression))),

    ternary_expression: ($) =>
      prec.right(PREC.ternary,
        seq(field("condition", $._single_expression), "?",
            field("consequence", $._single_expression), ":",
            field("alternative", $._single_expression))),

    binary_expression: ($) =>
      choice(
        // `??` sits with `||` and may not be mixed with it or `&&` without
        // parentheses — a rule the compiler enforces rather than a precedence,
        // so this grammar admits it and leaves the complaint to the checker.
        ...[
          ["??", PREC.or], ["||", PREC.or], ["&&", PREC.and],
          ["===", PREC.equality], ["!==", PREC.equality],
          ["<", PREC.relational], ["<=", PREC.relational],
          [">", PREC.relational], [">=", PREC.relational],
          ["instanceof", PREC.relational], ["in", PREC.relational],
          ["+", PREC.additive], ["-", PREC.additive],
          ["*", PREC.multiplicative], ["/", PREC.multiplicative],
          ["%", PREC.multiplicative],
        ].map(([operator, precedence]) =>
          prec.left(precedence,
            seq(field("left", $._single_expression),
                field("operator", operator),
                field("right", $._single_expression)))),
        // `**` is the one right-associative binary operator.
        prec.right(PREC.exponent,
          seq(field("left", $._single_expression), field("operator", "**"),
              field("right", $._single_expression))),
      ),

    unary_expression: ($) =>
      prec.right(PREC.unary,
        seq(field("operator", choice("!", "-", "+", "typeof", "await", "void", "delete")),
            field("argument", $._single_expression))),

    update_expression: ($) =>
      prec.right(PREC.unary,
        choice(seq(field("operator", choice("++", "--")), field("argument", $._single_expression)),
               seq(field("argument", $._single_expression), field("operator", choice("++", "--"))))),

    yield_expression: ($) =>
      prec.right(PREC.assignment,
        seq("yield", optional("*"), optional($._single_expression))),

    arrow_function: ($) =>
      prec.right(PREC.assignment,
        seq(optional(field("async", "async")),
            field("parameters", choice($.identifier, $.formal_parameters)),
            optional($.type_annotation),
            "=>",
            field("body", choice($._single_expression, $.statement_block)))),

    function_expression: ($) =>
      seq(optional(field("async", "async")), "function", optional(field("generator", "*")),
          optional(field("name", $.identifier)),
          field("parameters", $.formal_parameters),
          field("body", $.statement_block)),

    class_expression: ($) =>
      seq("class", optional(field("name", $.identifier)),
          optional(seq("extends", field("superclass", $.identifier))),
          field("body", $.class_body)),

    primary_expression: ($) =>
      choice(
        $.number, $.bigint, $.string, $.template_string, $.regex,
        $.identifier, $.this, $.super, $.new_target,
        $.true, $.false, $.null, $.undefined,
        $.array, $.object,
        $.new_expression, $.call_expression, $.member_expression,
        $.subscript_expression, $.parenthesized_expression, $.dynamic_import,
      ),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    member_expression: ($) =>
      prec(PREC.member,
        seq(field("object", $._single_expression),
            field("operator", choice(".", "?.")),
            field("property", choice($.identifier, $.private_property_identifier)))),

    subscript_expression: ($) =>
      prec.right(PREC.member,
        seq(field("object", $._single_expression),
            optional("?."), "[", field("index", $._single_expression), "]")),

    call_expression: ($) =>
      prec(PREC.call,
        seq(field("function", $._single_expression),
            optional("?."),
            choice(field("arguments", $.arguments),
                   field("template", $.template_string)))),

    // `import(specifier)` is its own form: `import` is a keyword, so this is
    // not a call of anything.
    dynamic_import: ($) => seq("import", "(", $._single_expression, ")"),

    new_expression: ($) =>
      prec.right(PREC.call,
        seq("new", field("constructor", $._single_expression),
            optional(field("arguments", $.arguments)))),

    new_target: () => seq("new", ".", "target"),

    arguments: ($) => seq("(", commaSep($._argument), ")"),
    _argument: ($) => choice($.spread_element, $._single_expression),
    spread_element: ($) => seq("...", $._single_expression),

    array: ($) => seq("[", commaSep($._argument), "]"),

    object: ($) => seq("{", commaSep($._object_entry), "}"),
    _object_entry: ($) =>
      choice($.pair, $.shorthand_property, $.object_method, $.object_accessor,
             $.spread_element),
    pair: ($) =>
      seq(field("key", $._property_name), ":", field("value", $._single_expression)),
    shorthand_property: ($) => field("key", $.identifier),
    object_method: ($) =>
      seq(optional(field("async", "async")), optional(field("generator", "*")),
          field("name", $._property_name),
          field("parameters", $.formal_parameters), field("body", $.statement_block)),
    object_accessor: ($) =>
      seq(field("accessor", choice("get", "set")), field("name", $._property_name),
          field("parameters", $.formal_parameters), field("body", $.statement_block)),

    // ---- terminals -------------------------------------------------------

    identifier: () => /[A-Za-z_$][A-Za-z0-9_$]*/,
    private_property_identifier: () => /#[A-Za-z_$][A-Za-z0-9_$]*/,

    this: () => "this",
    super: () => "super",
    true: () => "true",
    false: () => "false",
    null: () => "null",
    undefined: () => "undefined",

    // A BigInt is a different type rather than a wider number, so it is a
    // token of its own and not a number with a suffix.
    bigint: () =>
      token(seq(choice(/0[xX][0-9a-fA-F][0-9a-fA-F_]*/, /0[oO][0-7][0-7_]*/,
                       /0[bB][01][01_]*/, /\d[\d_]*/), "n")),

    number: () =>
      token(choice(/0[xX][0-9a-fA-F][0-9a-fA-F_]*/, /0[oO][0-7][0-7_]*/,
                   /0[bB][01][01_]*/,
                   /(\d[\d_]*)?\.\d[\d_]*([eE][+-]?\d+)?/,
                   /\d[\d_]*\.?([eE][+-]?\d+)?/)),

    string: ($) =>
      choice(
        seq('"', repeat(choice($.escape_sequence, /[^"\\\n]+/)), '"'),
        seq("'", repeat(choice($.escape_sequence, /[^'\\\n]+/)), "'"),
      ),

    template_string: ($) =>
      seq("`", repeat(choice($.escape_sequence, $.template_substitution, /[^`$\\]+/, "$")), "`"),
    template_substitution: ($) => seq("${", $._expression, "}"),

    escape_sequence: () =>
      token(seq("\\", choice(/u\{[0-9a-fA-F]+\}/, /u[0-9a-fA-F]{4}/,
                             /x[0-9a-fA-F]{2}/, /[^ux]/))),

    regex: ($) =>
      seq("/", field("pattern", $.regex_pattern), "/", optional(field("flags", $.regex_flags))),
    regex_pattern: () =>
      token.immediate(
        repeat1(choice(seq("[", repeat(choice(seq("\\", /./), /[^\]\n\\]/)), "]"),
                       seq("\\", /./), /[^/\\\[\n]/))),
    regex_flags: () => token.immediate(/[dgimsuvy]+/),

    comment: () =>
      token(choice(seq("//", /[^\n]*/), seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/"))),
  },
});

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}
