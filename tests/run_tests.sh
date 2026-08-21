#!/usr/bin/env bash
# Golden-file test runner.
#
# Every tests/cases/NAME.cx is executed and its combined stdout+stderr compared
# against tests/cases/NAME.expected. A case whose name starts with "error_" is
# expected to exit non-zero; every other case must exit 0.
#
# A case may also be a directory, for programs that span files:
# tests/cases/NAME/main.cx is the entry point and tests/cases/NAME/expected
# holds the output.
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

# Cases are grouped by role — language/, library/, errors/ and so on — and a
# case may be a single file or a directory with a main.cx in it. A directory
# holding main.cx *is* a case; anything else is a group to look inside. That
# rule is what lets the two kinds live in the same tree without a manifest.
cases=()
while IFS= read -r entry; do
  cases+=("$entry")
done < <(
  find "$ROOT/tests/cases" -name 'main.cx' -print
  # Every other program, minus the ones a directory case owns. That means any
  # ancestor holding a main.cx, not only the immediate one: a multi-file case
  # may keep its parts in a lib/ of its own.
  find "$ROOT/tests/cases" -name '*.cx' ! -name 'main.cx' -print |
    while IFS= read -r candidate; do
      owned=0
      probe="$(dirname "$candidate")"
      while [[ "$probe" != "$ROOT/tests/cases" && "$probe" != "/" ]]; do
        if [[ -f "$probe/main.cx" ]]; then owned=1; break; fi
        probe="$(dirname "$probe")"
      done
      [[ $owned -eq 1 ]] || printf '%s\n' "$candidate"
    done
)

for case_file in "${cases[@]}"; do
  if [[ "$(basename "$case_file")" == "main.cx" ]]; then
    name="$(basename "$(dirname "$case_file")")"
    expected_file="$(dirname "$case_file")/expected"
  else
    name="$(basename "$case_file" .cx)"
    expected_file="${case_file%.cx}.expected"
  fi

  if [[ -n "$FILTER" && "$name" != *"$FILTER"* ]]; then
    skipped=$((skipped + 1))
    continue
  fi

  actual="$("$BIN" "$case_file" 2>&1)"
  status=$?

  # Paths appear in diagnostics; strip the directory so results are portable.
  # Both spellings, because a path is shown relative to the working directory
  # when it sits underneath it and absolute when it does not.
  actual="${actual//$ROOT\/tests\/cases\//}"
  actual="${actual//tests\/cases\//}"
  # And the group the case is filed under, so moving a case between groups does
  # not rewrite what it is expected to print.
  case_dir_rel="$(dirname "${case_file#$ROOT/tests/cases/}")"
  [[ "$case_dir_rel" != "." ]] && actual="${actual//$case_dir_rel\//}"
  actual="${actual//$name\//}"

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
