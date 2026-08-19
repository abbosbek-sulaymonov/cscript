#!/usr/bin/env bash
# Node parity check.
#
# CScript's syntax is a subset of TypeScript's, so every example is handed to
# Node as a .ts file with --experimental-strip-types, which erases the
# annotations and runs the JavaScript underneath. That checks two claims at
# once: the syntax really is TypeScript, and the behaviour really does match
# once the types are gone.
#
# Most examples must produce byte-identical output. The exceptions are the
# files that exist to demonstrate a deliberate semantic difference, listed in
# DIVERGENT below — those are required to differ, so a fix that quietly stops
# working fails the build too.
#
# Skips cleanly when Node is too old or absent — this is a signal, not a build
# dependency.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/cscript}"

# Examples whose whole point is to behave differently from JavaScript.
DIVERGENT=("fixes")

if ! command -v node >/dev/null 2>&1; then
  echo "node not installed — skipping parity check"
  exit 0
fi

# Type stripping landed in Node 22.6. Without it the annotations are syntax
# errors and the comparison would be meaningless rather than merely absent.
if ! node --experimental-strip-types --eval 'let x: number = 1;' >/dev/null 2>&1; then
  echo "node lacks --experimental-strip-types (needs 22.6+) — skipping parity check"
  exit 0
fi

run_node() { node --experimental-strip-types --no-warnings "$1" 2>&1; }

if [[ ! -x "$BIN" ]]; then
  echo "parity: '$BIN' not built — run 'make' first" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass=0; fail=0; divergent=0

# An example is either one .cx file or a directory whose main.cx is the entry
# point. A directory is copied whole so its imports still resolve, with .cx
# renamed to .ts in both filenames and specifiers.
examples=()
for entry in "$ROOT"/examples/*.cx; do
  [[ -e "$entry" ]] && examples+=("$entry")
done
for entry in "$ROOT"/examples/*/; do
  [[ -f "$entry/main.cx" ]] && examples+=("$entry/main.cx")
done

for example in "${examples[@]}"; do
  if [[ "$(basename "$example")" == "main.cx" ]]; then
    name="$(basename "$(dirname "$example")")"
    is_directory=1
  else
    name="$(basename "$example" .cx)"
    is_directory=0
  fi

  is_divergent=0
  for entry in "${DIVERGENT[@]}"; do
    [[ "$name" == "$entry" ]] && is_divergent=1
  done

  if [[ $is_directory -eq 1 ]]; then
    rm -rf "$tmp/$name"
    cp -R "$(dirname "$example")" "$tmp/$name"
    while IFS= read -r file; do
      sed 's/\.cx"/.ts"/g' "$file" > "${file%.cx}.ts"
      rm "$file"
    done < <(find "$tmp/$name" -name '*.cx')
    entry="$tmp/$name/main.ts"
  else
    cp "$example" "$tmp/$name.ts"
    entry="$tmp/$name.ts"
  fi

  ours="$("$BIN" "$example" 2>&1)"
  theirs="$(run_node "$entry")"

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
