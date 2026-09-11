#!/usr/bin/env bash
# Runs a CScript program under Node, with `std:` imports pointed at the library.
#
# Node has no idea what `std:iter` means, so the program and every library
# module are copied to a scratch directory as .ts files and the specifiers are
# rewritten to relative paths. Node's --experimental-strip-types then erases
# the annotations and runs the JavaScript underneath.
#
# This is what keeps Node the oracle for library/: the expected output of every
# tests/cases/stdlib case was produced by this script, and tests/node_parity.sh
# uses it to keep checking that the two agree.
#
#   tests/std_node.sh tests/cases/stdlib/iter.cx
#
# Prints the program's combined stdout and stderr. Exits 2 when Node is absent
# or too old, so a caller can tell "cannot check" from "did not match".

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROGRAM="${1:-}"

if [[ -z "$PROGRAM" ]]; then
  echo "usage: tests/std_node.sh <program.cx>" >&2
  exit 64
fi
if [[ ! -f "$PROGRAM" ]]; then
  echo "std_node: '$PROGRAM' is not a file" >&2
  exit 66
fi

if ! command -v node >/dev/null 2>&1; then
  echo "node not installed" >&2
  exit 2
fi
if ! node --experimental-strip-types --eval 'let x: number = 1;' >/dev/null 2>&1; then
  echo "node lacks --experimental-strip-types (needs 22.6+)" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# `std:name` becomes `./std_name.ts`, and each library module is copied under
# that name. Flat, because a library module may import another one and the two
# then have to agree about where they are.
for module in "$ROOT"/library/*/*.cx; do
  [[ -e "$module" ]] || continue
  name="$(basename "$module" .cx)"
  sed -E 's|"std:([a-zA-Z0-9_]+)"|"./std_\1.ts"|g' "$module" > "$tmp/std_$name.ts"
done

sed -E 's|"std:([a-zA-Z0-9_]+)"|"./std_\1.ts"|g' "$PROGRAM" > "$tmp/program.ts"

node --experimental-strip-types --no-warnings "$tmp/program.ts" 2>&1
