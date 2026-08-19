#!/usr/bin/env bash
# Benchmark runner.
#
#   bench/run.sh                 time every bench/*.cx
#   bench/run.sh loop            only those matching "loop"
#   REPS=5 bench/run.sh          more repetitions (default 3)
#   BASELINE=out.txt bench/run.sh > new.txt   then diff to see a regression
#
# Reports the best of REPS runs, because the minimum is the measurement least
# polluted by scheduling noise. Also times Node on the same source when it is
# installed — CScript's syntax is a subset of JavaScript's, so the .cx file is
# handed to Node unchanged.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/cscript}"
REPS="${REPS:-3}"
FILTER="${1:-}"

if [[ ! -x "$BIN" ]]; then
  echo "bench: '$BIN' not built — run 'make' first" >&2
  exit 1
fi

# `time` in the shell only has 10 ms resolution here, so measure in Python.
now() { python3 -c 'import time; print(time.perf_counter())'; }

best_ms() {
  local best=""
  for ((rep = 0; rep < REPS; rep++)); do
    local start stop ms
    start="$(now)"
    "$@" >/dev/null 2>&1 || return 1
    stop="$(now)"
    ms="$(python3 -c "print(round((${stop}-${start})*1000))")"
    if [[ -z "$best" || "$ms" -lt "$best" ]]; then best="$ms"; fi
  done
  printf '%s' "$best"
}

have_node=0
command -v node >/dev/null 2>&1 && have_node=1

printf '%-16s %12s %12s %10s\n' "benchmark" "cscript" "node" "ratio"
printf -- '---------------------------------------------------------\n'

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

total_cs=0
for program in "$ROOT"/bench/*.cx; do
  name="$(basename "$program" .cx)"
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue

  cs="$(best_ms "$BIN" "$program")" || { printf '%-16s %12s\n' "$name" "FAILED"; continue; }
  total_cs=$((total_cs + cs))

  if [[ $have_node -eq 1 ]]; then
    cp "$program" "$tmp/$name.js"
    nd="$(best_ms node "$tmp/$name.js")" || nd=""
    if [[ -n "$nd" && "$nd" -gt 0 ]]; then
      ratio="$(python3 -c "print(f'{${cs}/${nd}:.1f}x')")"
    else
      ratio="-"
    fi
    printf '%-16s %10s ms %10s ms %10s\n' "$name" "$cs" "${nd:--}" "$ratio"
  else
    printf '%-16s %10s ms %12s %10s\n' "$name" "$cs" "-" "-"
  fi
done

printf -- '---------------------------------------------------------\n'
printf '%-16s %10s ms\n' "total" "$total_cs"

# Native reference points, built on demand, for the primary arithmetic loop.
if [[ -z "$FILTER" || "loop_arith" == *"$FILTER"* ]]; then
  echo
  echo "native reference (bench/native/loop_arith.*):"
  if command -v cc >/dev/null 2>&1; then
    cc -O2 -o "$tmp/loop_c" "$ROOT/bench/native/loop_arith.c" 2>/dev/null &&
      printf '  %-14s %10s ms\n' "C -O2" "$(best_ms "$tmp/loop_c")"
  fi
  if command -v go >/dev/null 2>&1; then
    (cd "$tmp" && go build -o "$tmp/loop_go" "$ROOT/bench/native/loop_arith.go" 2>/dev/null) &&
      printf '  %-14s %10s ms\n' "Go" "$(best_ms "$tmp/loop_go")"
  fi
fi
