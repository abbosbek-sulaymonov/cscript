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

# Timing runs inside one Python process rather than shelling out for a clock.
#
# The previous version called python3 twice per repetition to read the clock,
# which put that second interpreter's own startup — about 19 ms — inside every
# measurement it took. A constant offset like that does not just add noise: it
# flatters whichever program is slower, because it is a smaller share of a
# bigger number, and it made the Node comparison look far closer than it is.
best_ms() {
  python3 - "$@" <<'PYTHON'
import subprocess, sys, time
reps = int(__import__("os").environ.get("REPS", "3"))
best = None
for _ in range(reps):
    started = time.perf_counter()
    done = subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    elapsed = (time.perf_counter() - started) * 1000
    if done.returncode != 0:
        sys.exit(1)
    if best is None or elapsed < best:
        best = elapsed
print(round(best))
PYTHON
}

have_node=0
command -v node >/dev/null 2>&1 && have_node=1

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Startup, measured and then subtracted.
#
# Node takes about 37 ms to start and CScript about 2. On a benchmark that runs
# for 200 ms that is a fifth of the total, and on a short one it is most of it
# — so a table of end-to-end times flatters whichever interpreter starts
# faster and says very little about either one's speed. Both numbers are
# reported: the one a user of a CLI feels, and the one that is actually about
# the interpreter.
: > "$tmp/empty.js"
: > "$tmp/empty.cx"
cs_startup="$(best_ms "$BIN" "$tmp/empty.cx")"
node_startup=0
[[ $have_node -eq 1 ]] && node_startup="$(best_ms node "$tmp/empty.js")"

printf 'startup: cscript %s ms, node %s ms — subtracted from the compute column\n\n' \
  "$cs_startup" "$node_startup"
printf '%-14s %11s %10s %11s %10s %8s\n' \
  "benchmark" "cscript" "node" "cscript" "node" "ratio"
printf '%-14s %11s %10s %11s %10s %8s\n' \
  "" "(total)" "(total)" "(compute)" "(compute)" ""
printf -- '---------------------------------------------------------------------\n'

total_cs=0
for program in "$ROOT"/bench/*.cx; do
  name="$(basename "$program" .cx)"
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue

  cs="$(best_ms "$BIN" "$program")" || { printf '%-16s %12s\n' "$name" "FAILED"; continue; }
  total_cs=$((total_cs + cs))

  if [[ $have_node -eq 1 ]]; then
    cp "$program" "$tmp/$name.js"
    nd="$(best_ms node "$tmp/$name.js")" || nd=""
    cs_compute=$((cs - cs_startup))
    nd_compute=$((nd - node_startup))
    (( cs_compute < 1 )) && cs_compute=1
    (( nd_compute < 1 )) && nd_compute=1

    if [[ -n "$nd" && "$nd" -gt 0 ]]; then
      ratio="$(python3 -c "print(f'{${cs_compute}/${nd_compute}:.1f}x')")"
    else
      ratio="-"
    fi
    printf '%-14s %8s ms %7s ms %8s ms %7s ms %8s\n' \
      "$name" "$cs" "${nd:--}" "$cs_compute" "$nd_compute" "$ratio"
  else
    printf '%-14s %8s ms %10s %11s %10s %8s\n' "$name" "$cs" "-" "-" "-" "-"
  fi
done

printf -- '---------------------------------------------------------------------\n'
printf '%-14s %8s ms\n' "total" "$total_cs"

# Native reference points, built on demand, for the primary arithmetic loop.
if [[ -z "$FILTER" || "loop_arith" == *"$FILTER"* ]]; then
  echo
  echo "native reference (bench/native/loop_arith.*), total time:"
  if command -v cc >/dev/null 2>&1; then
    cc -O2 -o "$tmp/loop_c" "$ROOT/bench/native/loop_arith.c" 2>/dev/null &&
      printf '  %-14s %10s ms\n' "C -O2" "$(best_ms "$tmp/loop_c")"
  fi
  if command -v go >/dev/null 2>&1; then
    (cd "$tmp" && go build -o "$tmp/loop_go" "$ROOT/bench/native/loop_arith.go" 2>/dev/null) &&
      printf '  %-14s %10s ms\n' "Go" "$(best_ms "$tmp/loop_go")"
  fi
fi
