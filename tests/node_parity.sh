#!/usr/bin/env bash
# Node parity check.
#
# CScript's syntax is a subset of JavaScript's, so every example must at least
# *parse and run* under Node. Most must also produce byte-identical output;
# the exceptions are the files that exist to demonstrate a deliberate semantic
# fix, listed in DIVERGENT below.
#
# Skips cleanly when Node is not installed — this is a nice-to-have signal, not
# a build dependency.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/cscript}"

# Examples whose whole point is to behave differently from JavaScript.
DIVERGENT=("fixes")

if ! command -v node >/dev/null 2>&1; then
  echo "node not installed — skipping parity check"
  exit 0
fi

if [[ ! -x "$BIN" ]]; then
  echo "parity: '$BIN' not built — run 'make' first" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass=0; fail=0; divergent=0

for example in "$ROOT"/examples/*.cx; do
  name="$(basename "$example" .cx)"

  is_divergent=0
  for entry in "${DIVERGENT[@]}"; do
    [[ "$name" == "$entry" ]] && is_divergent=1
  done

  cp "$example" "$tmp/$name.js"
  ours="$("$BIN" "$example" 2>&1)"
  theirs="$(node "$tmp/$name.js" 2>&1)"

  if [[ $is_divergent -eq 1 ]]; then
    if [[ "$ours" == "$theirs" ]]; then
      echo "FAIL       $name.cx (expected to differ from JavaScript, but matched)"
      fail=$((fail + 1))
    else
      echo "divergent  $name.cx (deliberate)"
      diff <(printf '%s\n' "$theirs") <(printf '%s\n' "$ours") \
        | grep -E '^[<>]' | sed 's/^/             /'
      divergent=$((divergent + 1))
    fi
    continue
  fi

  if [[ "$ours" == "$theirs" ]]; then
    echo "identical  $name.cx"
    pass=$((pass + 1))
  else
    echo "FAIL       $name.cx (output differs from Node)"
    diff <(printf '%s\n' "$theirs") <(printf '%s\n' "$ours") | sed 's/^/             /' | head -12
    fail=$((fail + 1))
  fi
done

echo
echo "-------------------------------------------"
echo "identical to Node: $pass, deliberately divergent: $divergent, failed: $fail"
[[ $fail -gt 0 ]] && exit 1
exit 0
