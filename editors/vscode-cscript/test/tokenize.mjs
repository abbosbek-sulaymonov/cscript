// Tokenises real CScript files with the shipped grammar and reports anything
// that came out unscoped. A grammar is only as good as what it does to the
// programs the compiler already accepts, so those are what it is run against.
import { readFileSync } from "node:fs";
import { createRequire as _cr } from "node:module";

const require = _cr(import.meta.url);
// Both packages are CommonJS, so they come in through require rather than a
// namespace import — which is what makes their functions actually appear.
const oniguruma = require("vscode-oniguruma");
const textmate = require("vscode-textmate");
const wasm = readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
await oniguruma.loadWASM(wasm.buffer);

const registry = new textmate.Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (sources) => new oniguruma.OnigScanner(sources),
    createOnigString: (s) => new oniguruma.OnigString(s),
  }),
  loadGrammar: async () =>
    textmate.parseRawGrammar(
      readFileSync("syntaxes/cscript.tmLanguage.json", "utf8"),
      "cscript.tmLanguage.json",
    ),
});

const grammar = await registry.loadGrammar("source.cscript");
let files = 0;
let tokens = 0;
let bare = 0;
const examples = new Map();

for (const path of process.argv.slice(2)) {
  files++;
  let rules = textmate.INITIAL;
  for (const line of readFileSync(path, "utf8").split("\n")) {
    const result = grammar.tokenizeLine(line, rules);
    rules = result.ruleStack;
    for (const token of result.tokens) {
      const text = line.slice(token.startIndex, token.endIndex);
      if (!text.trim()) continue;
      tokens++;
      // "source.cscript" alone means nothing matched it.
      if (token.scopes.length === 1) {
        bare++;
        if (!examples.has(text)) examples.set(text, `${path}: ${line.trim()}`);
      }
    }
  }
}

console.log(`${files} files, ${tokens} tokens, ${bare} unscoped`);
if (bare > 0) {
  console.log("unscoped text, most distinct first:");
  for (const [text, where] of [...examples].slice(0, 15)) {
    console.log(`  ${JSON.stringify(text)}  ${where.slice(0, 90)}`);
  }
}
