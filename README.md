<img src="assets/cscript-logo.svg" alt="CScript" height="64">

A **gradually typed** language with **TypeScript's syntax and none of
JavaScript's footguns**, implemented from scratch in C11 as a bytecode virtual
machine.

```js
// hello.cx — this is also a valid JavaScript file
for (let i = 1; i <= 15; i++) {
  if (i % 15 === 0) {
    console.log("FizzBuzz");
  } else if (i % 3 === 0) {
    console.log("Fizz");
  } else if (i % 5 === 0) {
    console.log("Buzz");
  } else {
    console.log(i);
  }
}
```

```
$ cscript examples/fizzbuzz.cx
1
2
Fizz
...
```

---
## The idea

**Every CScript program is a valid TypeScript program.** Same keywords, same
operators, same precedence, same `console.log`, same `: number` annotations.
`make test-node` hands each example to Node with `--experimental-strip-types`
and diffs the output, so that claim stays mechanically enforced rather than
aspirational.

**Types are optional, and inferred when you leave them off.**

```ts
const name: string = "cscript";   // annotated
let year = 2026;                  // inferred as number — just as checked
let loose: any = 1;               // opted out

let total: number = "text";   // error: cannot assign string to 'total'
true * 3;                     // error: operand of '*' must be a number
```

Those are **compile** errors now, not runtime ones — and the last needs no
annotation at all.

**The semantics are not JavaScript's.** CScript removes the traps that make
JavaScript hard to get right — and every removal is a **loud error**, never a
silent change of meaning. A program that would behave differently here refuses
to compile or fails at runtime, so nothing you write can quietly mean one thing
in CScript and another in Node.

```js
1 == "1"      // error: '==' coerces its operands; use '==='
var x = 1;    // error: 'var' is function-scoped and hoisted; use 'let' or 'const'
true * 3      // runtime error: operands of '*' must be numbers
console.log(typo);  // runtime error: 'typo' is not defined
answer = 43;  // error: 'answer' is declared const and cannot be reassigned
typeof null   // "null", not "object"
```

The full list, with the reasoning for each, is in
[docs/GRAMMAR.md](docs/GRAMMAR.md#fixes-to-javascript).

---
## Status

**v0.46.0.** The language is feature-complete for everyday code and well past
it: objects and arrays, functions and closures, classes with private members
and static blocks, modules, promises and `async`/`await`, generators and async
generators, regular expressions, `Map`/`Set`, `Symbol`, `BigInt`, `Date`,
prototypes, gradual typing with inference, and the whole of the control flow.

The pipeline is lexer → parser → type checker → bytecode compiler → stack VM,
with a mark-sweep collector underneath. Above it sits a second tier: hot
functions are lowered to a typed IR and compiled to arm64 machine code, entered
either at a call or part-way through a running loop, with small callees spliced
into it. On the loops it takes, that is **8–24× the interpreter** — see
[Performance](docs/PERFORMANCE.md).

```ts
class Person {
  constructor(name: string, born: number) {
    this.name = name;
    this.born = born;
  }

  label(): string {
    return `${this.name} (${this.born})`;
  }
}

const people = [new Person("Ada", 1815), new Person("Alan", 1912)];
const names = people.map(p => p.name).filter(n => n.length > 3).sort();

try {
  console.log(JSON.stringify({ names: names, first: people[0].label() }));
} catch (e) {
  console.log(e.message);
}
```

What remains is mostly compiler work rather than language work — calls and
allocation inside compiled code — and a short list of deliberate omissions.
Both are in the [roadmap](docs/ROADMAP.md).

---
## Build and run

No dependencies beyond a C11 compiler and `make`.

```bash
git clone https://github.com/abbosbek-sulaymonov/cscript.git
cd cscript
make
```

```bash
./build/release/cscript                       # REPL
./build/release/cscript examples/hello.cx     # run a file
./build/release/cscript -e 'console.log(6*7);'
./build/release/cscript --check src/*.cx      # compile and type-check only
./build/release/cscript --help
```

> Source files use **`.cx`**. `.cs` belongs to C#.

In the REPL, an entry that is one expression has its value printed, and one
that leaves a bracket open keeps reading. `--check` compiles and type-checks
without running, which is what an editor or a CI step wants; `--print-tokens`,
`--print-ast` and `--print-bytecode` show any stage of the pipeline for any
program; arguments after the script arrive in `process.argv`, spelled the way
Node spells it. `cscript --help` has the rest.

---

## Documentation

| Document | What is in it |
| --- | --- |
| [A tour of the language](docs/TOUR.md) | Enough to judge it, in one page |
| [The command line](docs/CLI.md) | Every option, the exit codes, and the REPL |
| [Grammar and semantics](docs/GRAMMAR.md) | The full syntax as EBNF, and what each construct means |
| [Against JavaScript](docs/JAVASCRIPT.md) | What is the same, what differs on purpose and why, what is missing |
| [Architecture](docs/ARCHITECTURE.md) | The pipeline, the object model, the collector, the compiler |
| [Performance](docs/PERFORMANCE.md) | The benchmarks, and the measurements behind each decision |
| [Roadmap](docs/ROADMAP.md) | What has been built, and what is next |
| [Working on it](docs/DEVELOPMENT.md) | Build modes, the test suites, and where everything lives |
| [The source map](src/README.md) | What is in `src/`, directory by directory, and the conventions every file follows |
| [The standard library](library/README.md) | The ten `std:` modules that ship with the language |
| [Editor support](editors/README.md) | VS Code, tree-sitter, and what GitHub does and does not know |

---

## Author

**Abbosbek Sulaymonov** — [@abbosbek-sulaymonov](https://github.com/abbosbek-sulaymonov) · [abbosbek.uz](https://abbosbek.uz)

## License

MIT — see [LICENSE](LICENSE).
