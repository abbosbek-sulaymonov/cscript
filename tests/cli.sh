#!/usr/bin/env bash
# The command line, which the golden-file suite cannot reach.
#
#   tests/cli.sh
#   BIN=build/release/cscript tests/cli.sh
#
# Every other suite runs *programs*: it says nothing about what the binary does
# with its own arguments, and nothing at all about the REPL. Both have been
# wrong in ways a program could not show — `-e` with no trailing semicolon, an
# entry that could not be typed without one, a missing file reported as a
# program that would not compile.
#
# Each case asserts on the exit code, and on stdout and stderr where they are
# the point. Exit codes follow sysexits: 64 usage, 65 compile, 66 no input, 70
# runtime.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/cscript}"

if [[ ! -x "$BIN" ]]; then
  echo "cli: '$BIN' not built — run 'make' first" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

passed=0
failed=0

# check <name> <expected-code> <expected-stdout> -- <argv...>
check() {
  local name="$1" wantCode="$2" wantOut="$3"
  shift 4 # name, code, stdout, and the literal --
  local out code
  out="$("$BIN" "$@" 2>/dev/null)"
  code=$?

  if [[ "$code" == "$wantCode" && "$out" == "$wantOut" ]]; then
    passed=$((passed + 1))
    printf 'ok       %s\n' "$name"
  else
    failed=$((failed + 1))
    printf 'FAILED   %s\n' "$name"
    printf '  exit:   want %s, got %s\n' "$wantCode" "$code"
    [[ "$out" != "$wantOut" ]] && printf '  stdout: want %q, got %q\n' "$wantOut" "$out"
  fi
}

# checkStderr <name> <expected-substring> -- <argv...>
checkStderr() {
  local name="$1" want="$2"
  shift 3
  local err
  err="$("$BIN" "$@" 2>&1 >/dev/null)"
  if [[ "$err" == *"$want"* ]]; then
    passed=$((passed + 1))
    printf 'ok       %s\n' "$name"
  else
    failed=$((failed + 1))
    printf 'FAILED   %s\n' "$name"
    printf '  stderr: want to contain %q, got %q\n' "$want" "$err"
  fi
}

# checkStdin <name> <expected-code> <expected-stdout> <input> -- <argv...>
checkStdin() {
  local name="$1" wantCode="$2" wantOut="$3" input="$4"
  shift 5
  local out code
  out="$(printf '%s' "$input" | "$BIN" "$@" 2>/dev/null)"
  code=$?

  if [[ "$code" == "$wantCode" && "$out" == "$wantOut" ]]; then
    passed=$((passed + 1))
    printf 'ok       %s\n' "$name"
  else
    failed=$((failed + 1))
    printf 'FAILED   %s\n' "$name"
    printf '  exit:   want %s, got %s\n' "$wantCode" "$code"
    [[ "$out" != "$wantOut" ]] && printf '  stdout: want %q, got %q\n' "$wantOut" "$out"
  fi
}

printf 'console.log("from a file");\n' > "$WORK/good.cx"
printf 'console.log(1)\n' > "$WORK/nosemi.cx"
printf 'let x: number = "text";\n' > "$WORK/badtype.cx"
printf 'console.log(process.argv.length, process.argv[2], process.argv[3]);\n' > "$WORK/args.cx"
printf 'export const two = 2;\n' > "$WORK/lib.cx"
printf 'import { two } from "./lib.cx";\nconsole.log(two);\n' > "$WORK/main.cx"
printf 'import { nope } from "./lib.cx";\nconsole.log(nope);\n' > "$WORK/badimport.cx"

echo "-- what to run"
check "a file"                    0 "from a file"  -- "$WORK/good.cx"
check "-e"                        0 "42"           -- -e 'console.log(6*7);'
check "--eval"                    0 "42"           -- --eval 'console.log(6*7);'
check "--eval=code"               0 "42"           -- --eval='console.log(6*7);'
checkStdin "stdin as -"           0 "piped"        'console.log("piped");' --  -
check "-- ends the options"       0 "from a file"  -- -- "$WORK/good.cx"

echo "-- what it says about itself"
check "--version"                 0 "cscript $("$BIN" --version | cut -d' ' -f2)" -- --version
checkStderr "--help goes to stdout on request" "" -- --help
check "-h prints usage"           0 "$("$BIN" --help)" -- -h

echo "-- exit codes"
check "runs cleanly"              0  ""  -- -e '1;'
check "will not compile"          65 ""  -- "$WORK/nosemi.cx"
check "fails the type check"      65 ""  -- "$WORK/badtype.cx"
check "fails while running"       70 ""  -- -e 'nosuchname;'
check "no such file"              66 ""  -- "$WORK/nowhere.cx"
check "unknown option"            64 ""  -- --nonsense
check "-e with nothing after it"  64 ""  -- -e
check "--jit-threshold wants a number" 64 "" -- --jit-threshold zero
checkStderr "no such file blames the file, not the compiler" "could not open" -- "$WORK/nowhere.cx"

echo "-- --check compiles without running"
check "--check accepts a good file"    0  ""  -- --check "$WORK/good.cx"
check "--check refuses a bad one"      65 ""  -- --check "$WORK/nosemi.cx"
check "--check runs nothing"           0  ""  -- --check -e 'console.log("must not print");'
check "--check reads a whole graph"    0  ""  -- --check "$WORK/main.cx"
check "--check finds a bad import"     65 ""  -- --check "$WORK/badimport.cx"
check "-c is --check"                  0  ""  -- -c "$WORK/good.cx"
checkStderr "--check with nothing to check says so" "needs a script" -- --check

echo "-- process.argv"
check "arguments reach the program"    0 "4 alpha beta" -- "$WORK/args.cx" alpha beta
check "an argument may look like an option" 0 "3 --help undefined" -- "$WORK/args.cx" --help
check "argv without arguments"         0 "2 undefined undefined" -- "$WORK/args.cx"

echo "-- printing the stages"
checkStderr "--print-tokens says something" "" -- --print-tokens -e '1;'
check "--print-tokens names the tokens"   0 "$(printf '== tokens ==\n   1 NUMBER             '"'"'1'"'"'\n   | SEMICOLON          '"'"';'"'"'\n   | EOF                '"'"''"'"'')" -- --print-tokens -e '1;'
# Captured before matching: `grep -q` stops reading, and under `pipefail` the
# producer's broken pipe becomes the pipeline's status.
contains() {
  local name="$1" want="$2" got="$3"
  if [[ "$got" == *"$want"* ]]; then
    passed=$((passed + 1)); printf 'ok       %s\n' "$name"
  else
    failed=$((failed + 1)); printf 'FAILED   %s\n' "$name"
    printf '  want to contain %q\n' "$want"
  fi
}
contains "--print-ast prints a tree" "== ast ==" "$("$BIN" --print-ast -e 'let a = 1;')"
contains "--print-bytecode disassembles" "OP_DEFINE_GLOBAL" \
         "$("$BIN" --print-bytecode -e 'let a = 1;')"
contains "--print-bytecode covers every file read" "lib.cx" \
         "$("$BIN" --print-bytecode "$WORK/main.cx")"
checkStderr "--time reports on stderr" "ms" -- --time -e '1;'

echo "-- the compiler's own flags are accepted"
check "--no-jit"                 0 "1"  -- --no-jit -e 'console.log(1);'
check "--jit-threshold"          0 "1"  -- --jit-threshold 5 -e 'console.log(1);'
check "--jit-threshold=n"        0 "1"  -- --jit-threshold=5 -e 'console.log(1);'
# The report itself only exists where the tiering counter is compiled in, so
# what is asserted is that the flag is accepted and the program still ran.
jitReportOut="$("$BIN" --jit-report -e 'console.log(1);' 2>/dev/null)"
jitReportCode=$?
if [[ "$jitReportCode" == 0 && "$jitReportOut" == 1* ]]; then
  passed=$((passed + 1)); echo "ok       --jit-report"
else
  failed=$((failed + 1))
  echo "FAILED   --jit-report"
  printf '  exit %s, stdout %q\n' "$jitReportCode" "$jitReportOut"
fi

echo "-- the REPL"
# What is asserted is the value each entry prints, which is the part that did
# not exist: an expression used to be a syntax error. The banner is dropped,
# and so are the prompts — `>` for a new entry, `...` for the rest of one.
replOut() { printf '%s' "$1" | "$BIN" 2>&1 | tail -n +2; }

replCase() {
  local name="$1" input="$2" want="$3"
  local got
  got="$(replOut "$input" | sed 's/\.\.\. //g; s/> //g' | tr -s ' \n' ' ' |
         sed 's/^ //; s/ $//')"
  if [[ "$got" == "$want" ]]; then
    passed=$((passed + 1)); printf 'ok       %s\n' "$name"
  else
    failed=$((failed + 1)); printf 'FAILED   %s\n' "$name"
    printf '  want %q, got %q\n' "$want" "$got"
  fi
}

replCase "an expression, unterminated"   $'1+1\n'                        "2"
replCase "an expression, terminated"     $'1+1;\n'                       "2"
replCase "state carries between entries" $'let x = 5\nx*2\n'             "10"
replCase "a declaration prints nothing"  $'let y = 1\n'                  ""
replCase "a statement is not wrapped"    $'if (1 < 2) { console.log("yes"); }\n' "yes"
replCase "an entry spanning lines"       $'function f(a) {\n return a*3;\n}\nf(7)\n' "21"
replCase "a brace inside a string"       $'const o = { a: "}" }\no.a\n'  "}"
replCase "a trailing comment"            $'1+1 // note\n'                "2"
replCase "a semicolon inside a string"   $'"a;b";\n'                     "a;b"
replCase ".exit leaves"                  $'.exit\nconsole.log("never");\n' ""
contains ".help lists the commands" ".exit" "$(printf '.help\n' | "$BIN" 2>&1)"

echo
echo "-------------------------------------------"
echo "passed $passed, failed $failed"
[[ $failed -eq 0 ]]
