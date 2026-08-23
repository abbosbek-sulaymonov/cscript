#!/usr/bin/env bash
# CScript against Rust, on the three benchmarks that have a Rust port.
#
# Startup is measured separately and subtracted, because at Node's ~40ms it
# swamps everything else and would otherwise be the only thing the table shows.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for f in loop_arith loop_arith_int calls methods hello; do
  rustc -O -o "bench/rust/$f" "bench/rust/$f.rs"
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
printf 'console.log(1);\n' > "$TMP/hello.mjs"

timer() {
  python3 - "$@" <<'PY'
import subprocess, sys, time
best = min(
    (lambda s: time.perf_counter() - s)(time.perf_counter())
    for _ in range(1)
)
times = []
for _ in range(7):
    start = time.perf_counter()
    subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, check=True)
    times.append(time.perf_counter() - start)
print(f"{min(times) * 1000:.1f}")
PY
}

rust_base=$(timer ./bench/rust/hello)
cs_base=$(timer ./build/jit/cscript examples/hello.cx)
node_base=$(timer node "$TMP/hello.mjs")

echo "startup:  rust ${rust_base}ms   cscript ${cs_base}ms   node ${node_base}ms"
echo
printf "%-18s %9s %9s %9s\n" "compute (ms)" rust cscript node
for b in loop_arith calls methods; do
  cp "bench/$b.cx" "$TMP/$b.mjs"
  r=$(timer "./bench/rust/$b")
  c=$(timer ./build/jit/cscript "bench/$b.cx")
  n=$(timer node "$TMP/$b.mjs")
  printf "%-18s %9.1f %9.1f %9.1f\n" "$b" \
    "$(echo "$r - $rust_base" | bc)" \
    "$(echo "$c - $cs_base" | bc)" \
    "$(echo "$n - $node_base" | bc)"
done
r=$(timer ./bench/rust/loop_arith_int)
printf "%-18s %9.1f\n" "loop_arith (i64)" "$(echo "$r - $rust_base" | bc)"
