// Spot-checks that specific text gets the scope it should. The tokeniser above
// proves nothing is left unscoped; this proves the scopes are the right ones.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const oniguruma = require("vscode-oniguruma");
const textmate = require("vscode-textmate");

await oniguruma.loadWASM(
  readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm")).buffer,
);
const registry = new textmate.Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (s) => new oniguruma.OnigScanner(s),
    createOnigString: (s) => new oniguruma.OnigString(s),
  }),
  loadGrammar: async () =>
    textmate.parseRawGrammar(
      readFileSync("syntaxes/cscript.tmLanguage.json", "utf8"),
      "cscript.tmLanguage.json",
    ),
});
const grammar = await registry.loadGrammar("source.cscript");

// [source line, the text to find, the scope it must carry]
const expectations = [
  ["const total: number = 1;", "number", "support.type.primitive"],
  ["let big = 42n;", "42n", "constant.numeric.bigint"],
  ["let plain = 42;", "42", "constant.numeric.decimal"],
  ["let hex = 0xffn;", "0xffn", "constant.numeric.bigint"],
  ["const r = /a(?<name>b)/g;", "(?<name>", "keyword.other.group.regexp"],
  ["const q = a / b / c;", "/", "keyword.operator.arithmetic"],
  ["class Vec extends Base {}", "Vec", "entity.name.type.class"],
  ["class Vec extends Base {}", "Base", "entity.other.inherited-class"],
  ["function* gen() {}", "gen", "entity.name.function"],
  ["async function go() {}", "go", "entity.name.function"],
  ["this.#count = 1;", "#count", "variable.other.property.private"],
  ["outer: for (;;) {}", "outer", "entity.name.label"],
  ["break outer;", "outer", "entity.name.label"],
  ["const f = () => 1;", "=>", "keyword.operator.arrow"],
  ["console.log(1);", "console", "support.class.builtin"],
  ["console.log(1);", "log", "entity.name.function"],
  ["if (a ?? b) {}", "??", "keyword.operator.logical"],
  ["const t = `x${y}z`;", "${", "punctuation.definition.template-expression.begin"],
  ["const t = `x${y}z`;", "y", "variable.other.readwrite"],
  ["// a comment", "//", "punctuation.definition.comment"],
  ["// a comment", "a comment", "comment.line"],
  ["static { init(); }", "static", "storage.modifier"],
  ["get [key]() {}", "get", "storage.type.accessor"],
  ["for await (const x of s) {}", "await", "keyword.control.flow.await"],
  ["yield* other();", "yield*", "keyword.control.flow.yield"],
  ["const n = new.target;", "new.target", "variable.language.new-target"],
];

let failures = 0;
for (const [line, needle, wanted] of expectations) {
  const { tokens } = grammar.tokenizeLine(line, textmate.INITIAL);
  const hit = tokens.find((t) => {
    const text = line.slice(t.startIndex, t.endIndex);
    return text.trim() === needle.trim() && t.scopes.some((s) => s.startsWith(wanted));
  });
  if (!hit) {
    failures++;
    const near = tokens
      .filter((t) => line.slice(t.startIndex, t.endIndex).includes(needle.trim().slice(0, 4)))
      .map((t) => `${JSON.stringify(line.slice(t.startIndex, t.endIndex))}=${t.scopes.at(-1)}`);
    console.log(`FAIL  ${JSON.stringify(needle)} in ${JSON.stringify(line)}`);
    console.log(`      wanted ${wanted}, got ${near.join(" ") || "nothing"}`);
  }
}
console.log(
  failures
    ? `\n${failures} of ${expectations.length} scope checks failed`
    : `all ${expectations.length} scope checks passed`,
);
process.exit(failures ? 1 : 0);
