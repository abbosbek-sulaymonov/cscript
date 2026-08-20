#!/usr/bin/env bash
# What the JIT backend is worth, on the benchmarks it can compile.
#
#   bench/jit.sh                 every bench/jit_*.cx
#   REPS=5 bench/jit.sh          more repetitions (default 3)
#
# Each program is run three ways in the *same* binary — the `jit` build — so
# the only difference is whether the compiler was allowed to fire:
#
#   interpreted  threshold raised past anything the program will reach
#   compiled     the default threshold
#   node         the same source, with the annotations stripped
#
# These benchmarks are annotated on purpose — the annotations are what let the
# compiler emit unboxed arithmetic with no guards — so Node gets them through
# --experimental-strip-types, exactly as the parity check does.
#
# Timing the same binary both ways is what makes this a measurement of the
# compiler rather than of the build configuration: the back-edge counter costs
# a few percent and is present in both columns.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/jit/cscript"
REPS="${REPS:-3}"

if [[ ! -x "$BIN" ]]; then
  echo "bench: '$BIN' not built — run 'make jit' first" >&2
  exit 1
fi

best_ms() {
  python3 - "$@" <<'PYTHON'
import os, subprocess, sys, time
reps = int(os.environ.get("REPS", "3"))
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
print(f"{best:.0f}")
PYTHON
}

# Startup is subtracted, because at these sizes Node's is a large share of a
# small number and would flatter whichever program is slower.
cs_start=$(best_ms "$BIN" -e '')
node_start=0
have_node=0
if command -v node >/dev/null 2>&1 &&
   node --experimental-strip-types --eval 'let x: number = 1;' >/dev/null 2>&1; then
  have_node=1
  node_start=$(best_ms node --experimental-strip-types --no-warnings -e '')
fi

# Node will only strip types from a file it believes is TypeScript.
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
echo "startup: cscript ${cs_start} ms, node ${node_start} ms — subtracted below"
echo
printf '%-14s %11s %11s %9s %11s %9s\n' \
  benchmark interpreted compiled speedup node "vs node"
echo "---------------------------------------------------------------------------"

for source in "$ROOT"/bench/jit_*.cx; do
  name=$(basename "$source" .cx)

  off=$(CS_JIT_THRESHOLD=2000000000 best_ms "$BIN" "$source")
  on=$(best_ms "$BIN" "$source")
  off=$((off - cs_start)); on=$((on - cs_start))

  # Same answer both ways, or the speed means nothing.
  a=$(CS_JIT_THRESHOLD=2000000000 "$BIN" "$source")
  b=$("$BIN" "$source")
  if [[ "$a" != "$b" ]]; then
    printf '%-14s  MISMATCH: interpreted %s, compiled %s\n' "$name" "$a" "$b"
    continue
  fi

  if (( have_node )); then
    ts="$staging/$name.ts"
    cp "$source" "$ts"
    nd=$(best_ms node --experimental-strip-types --no-warnings "$ts")
    nd=$((nd - node_start))
    if [[ "$a" != "$(node --experimental-strip-types --no-warnings "$ts")" ]]; then
      printf '%-14s  DIFFERS FROM NODE\n' "$name"
    fi
    printf '%-14s %8s ms %8s ms %8sx %8s ms %8sx\n' "$name" "$off" "$on" \
      "$(python3 -c "print(f'{$off/max($on,1):.1f}')")" "$nd" \
      "$(python3 -c "print(f'{$on/max($nd,1):.1f}')")"
  else
    printf '%-14s %8s ms %8s ms %8sx %11s %9s\n' "$name" "$off" "$on" \
      "$(python3 -c "print(f'{$off/max($on,1):.1f}')")" "-" "-"
  fi
done
