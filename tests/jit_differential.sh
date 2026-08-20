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
BIN="$ROOT/build/jit/cscript"

if [[ ! -x "$BIN" ]]; then
  echo "differential: '$BIN' not built — run 'make jit' first" >&2
  exit 1
fi

checked=0
failed=0
compiled=0

for source in "$ROOT"/tests/cases/*.cx "$ROOT"/tests/cases/*/main.cx \
              "$ROOT"/examples/*.cx "$ROOT"/bench/*.cx "$ROOT"/bench/jit/*.cx; do
  [[ -f "$source" ]] || continue
  name="$(basename "$(dirname "$source")")/$(basename "$source")"

  interpreted="$(CS_JIT_THRESHOLD=2000000000 "$BIN" "$source" 2>&1)"
  hot="$(CS_JIT_THRESHOLD=1 "$BIN" "$source" 2>&1)"
  checked=$((checked + 1))

  # How much the compiler actually took, so a run of agreements cannot be
  # mistaken for coverage.
  taken="$(CS_JIT_REPORT=1 CS_JIT_THRESHOLD=1 "$BIN" "$source" 2>&1 |
           grep -E 'answered without|taken over|handed back' |
           grep -oE '^  [0-9]+' | tr -d ' ' | paste -sd+ - | bc)"
  compiled=$((compiled + ${taken:-0}))

  if [[ "$interpreted" != "$hot" ]]; then
    failed=$((failed + 1))
    echo "DIFFERS  $name"
    diff <(printf '%s\n' "$interpreted") <(printf '%s\n' "$hot") |
      sed 's/^/         /' | head -8
  fi
done

echo "-------------------------------------------"
echo "checked $checked programs, $failed disagreed"
echo "the compiler answered $compiled calls, loops and exits across them"
[[ $failed -eq 0 ]]
