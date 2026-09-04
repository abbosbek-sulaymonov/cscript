#!/usr/bin/env bash
# The compiler must not change what a program prints.
#
#   tests/jit_differential.sh
#
# Runs every case twice in the *same* `jit` binary — once with the tiering
# threshold raised out of reach, once with it at one, so everything the
# compiler can take it takes — and requires the two to agree byte for byte.
#
# This is the check the golden files cannot make. A `.expected` file pins what
# the program prints; it says nothing about whether the compiled path and the
# interpreted path agree, and a side exit is exactly the kind of change that
# would show up as a disagreement rather than as a wrong answer.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Overridable so the same differential can be run against another build —
# `make test-jit-gc` points it at one that also collects on every allocation.
BIN="${BIN:-$ROOT/build/jit/cscript}"

if [[ ! -x "$BIN" ]]; then
  echo "differential: '$BIN' not built — run 'make jit' first" >&2
  exit 1
fi

# Which trees to walk, and the benchmarks are in by default because they are
# the programs the compiler most wants to take. A build too slow to finish them
# passes its own list — `make test-jit-gc` collects on every allocation, and a
# twenty-million-iteration benchmark under that never ends.
roots=()
for tree in "$@"; do
  [[ "$tree" == /* ]] && roots+=("$tree") || roots+=("$ROOT/$tree")
done
if (( ${#roots[@]} == 0 )); then
  roots=("$ROOT/tests/cases" "$ROOT/examples" "$ROOT/bench")
fi

checked=0
failed=0
compiled=0
touched=0
generated=0

# The cases are grouped by role and a case may be a directory, so this walks
# the tree rather than globbing two shapes of path. Every program is run, parts
# of a multi-file case included — running one on its own is harmless, and the
# alternative is a second copy of the ownership rule in run_tests.sh.
while IFS= read -r source; do
  [[ -f "$source" ]] || continue
  name="$(basename "$(dirname "$source")")/$(basename "$source")"

  interpreted="$(CS_JIT_THRESHOLD=2000000000 "$BIN" "$source" 2>&1)"
  hot="$(CS_JIT_THRESHOLD=1 "$BIN" "$source" 2>&1)"
  # Compiling on the very first call is the harshest setting, and for one kind
  # of lowering it is *too* harsh to be a test: a property read is lowered
  # against what its inline cache has seen, and at a threshold of one the cache
  # has not run yet. A second setting lets the sites warm first, so the
  # profile-dependent paths are actually reached rather than only agreed with.
  warm="$(CS_JIT_THRESHOLD=40 "$BIN" "$source" 2>&1)"
  checked=$((checked + 1))

  # How much the compiler actually took, so a run of agreements cannot be
  # mistaken for coverage.
  # Two tallies, because "answered without the interpreter" counts the IR
  # interpreter and the machine code together — so a backend that stopped
  # emitting anything would leave that number untouched. Which tier answered is
  # the thing a code generator's coverage actually rests on.
  taken=0
  machine=0
  for threshold in 1 40; do
    part="$(CS_JIT_REPORT=1 CS_JIT_THRESHOLD=$threshold "$BIN" "$source" 2>&1 |
            grep -E 'answered without|taken over|handed back' |
            grep -oE '^  [0-9]+' | tr -d ' ' | paste -sd+ - | bc)"
    taken=$((taken + ${part:-0}))
    # Both streams, and captured before it is searched. Splitting them is not
    # worth getting wrong — the program's own output is harmless noise here —
    # and piping into `grep -q` would be worse: under `pipefail` an
    # early-exiting grep makes the producer take a SIGPIPE, and the pipeline
    # then reports failure however well the match went.
    report="$(CS_JIT_REPORT=1 CS_JIT_THRESHOLD=$threshold "$BIN" "$source" 2>&1)"
    if [[ "$report" =~ [1-9][0-9]*" of "[0-9]+" compiled to machine code" ]]; then
      machine=1
    fi
  done
  compiled=$((compiled + taken))
  [[ $taken -gt 0 ]] && touched=$((touched + 1))
  [[ $machine -eq 1 ]] && generated=$((generated + 1))

  for variant in hot warm; do
    other="${!variant}"
    [[ "$interpreted" == "$other" ]] && continue
    failed=$((failed + 1))
    echo "DIFFERS  $name ($variant)"
    diff <(printf '%s\n' "$interpreted") <(printf '%s\n' "$other") |
      sed 's/^/         /' | head -8
    break
  done
done < <(find "${roots[@]}" -name '*.cx' -print)

echo "-------------------------------------------"
echo "checked $checked programs, $failed disagreed"
echo "the compiler took part in $touched of them, answering $compiled calls,"
echo "loops and exits, and reached machine code in $generated — a drop in any of"
echo "those is a coverage regression even when nothing disagrees, which is how"
echo "one went unnoticed once"
[[ $failed -eq 0 ]]
