#!/usr/bin/env bash
# Golden-file test runner.
#
# Every tests/cases/NAME.cs is executed and its combined stdout+stderr compared
# against tests/cases/NAME.expected. A case whose name starts with "error_" is
# expected to exit non-zero; every other case must exit 0.
#
#   tests/run_tests.sh              use build/cscript
#   BIN=build/cscript-debug tests/run_tests.sh
#   tests/run_tests.sh string       run only cases matching "string"
#   UPDATE=1 tests/run_tests.sh     rewrite .expected from actual output

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cscript}"
FILTER="${1:-}"
UPDATE="${UPDATE:-0}"

if [[ ! -x "$BIN" ]]; then
  echo "test runner: '$BIN' not built — run 'make' first" >&2
  exit 1
fi

# LeakSanitizer only exists on Linux; asking for it on macOS makes ASan abort
# before the program runs, so only enable it where it is actually supported.
if [[ "$(uname -s)" == "Linux" ]]; then
  export ASAN_OPTIONS="detect_leaks=1"
else
  export ASAN_OPTIONS="detect_leaks=0"
fi

pass=0; fail=0; skipped=0
failed_names=()

for case_file in "$ROOT"/tests/cases/*.cs; do
  [[ -e "$case_file" ]] || continue
  name="$(basename "$case_file" .cs)"

  if [[ -n "$FILTER" && "$name" != *"$FILTER"* ]]; then
    skipped=$((skipped + 1))
    continue
  fi

  expected_file="${case_file%.cs}.expected"
  actual="$("$BIN" "$case_file" 2>&1)"
  status=$?

  # Paths appear in diagnostics; strip the directory so results are portable.
  actual="${actual//$ROOT\/tests\/cases\//}"

  if [[ "$UPDATE" == "1" ]]; then
    printf '%s\n' "$actual" > "$expected_file"
    echo "updated  $name"
    continue
  fi

  if [[ ! -f "$expected_file" ]]; then
    echo "MISSING  $name (no .expected file)"
    failed_names+=("$name"); fail=$((fail + 1)); continue
  fi

  expected="$(cat "$expected_file")"

  # Cases named error_* assert the failure path; everything else must succeed.
  if [[ "$name" == error_* ]]; then
    if [[ $status -eq 0 ]]; then
      echo "FAIL     $name (expected a non-zero exit, got 0)"
      failed_names+=("$name"); fail=$((fail + 1)); continue
    fi
  elif [[ $status -ne 0 ]]; then
    echo "FAIL     $name (exited $status)"
    printf '%s\n' "$actual" | sed 's/^/           /'
    failed_names+=("$name"); fail=$((fail + 1)); continue
  fi

  if [[ "$actual" == "$expected" ]]; then
    echo "ok       $name"
    pass=$((pass + 1))
  else
    echo "FAIL     $name"
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") \
      | sed 's/^/           /' | head -20
    failed_names+=("$name"); fail=$((fail + 1))
  fi
done

echo
echo "-------------------------------------------"
printf 'passed %d, failed %d' "$pass" "$fail"
[[ $skipped -gt 0 ]] && printf ', skipped %d' "$skipped"
printf '\n'

if [[ $fail -gt 0 ]]; then
  echo "failing: ${failed_names[*]}"
  exit 1
fi
