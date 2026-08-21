; What to hand to another grammar.
;
; A regular expression is the only one CScript has: a template literal holds
; CScript expressions, which this grammar already parses, so injecting into one
; would be asking the same parser to run twice over the same text.

((regex_pattern) @injection.content
  (#set! injection.language "regex"))

; A comment that opens with a doc marker is documentation, not prose to leave
; alone — where an editor has a jsdoc grammar it may as well use it.
((comment) @injection.content
  (#lua-match? @injection.content "^/[*][*]")
  (#set! injection.language "jsdoc"))
