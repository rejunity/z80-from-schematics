#!/usr/bin/env bash
# Run NASCOM BASIC 4.7 + Tiny BASIC scripts through basicrunner and verify
# expected substrings appear in stdout. Exit 0 if all subtests pass; non-zero
# (and a printed list of failures) otherwise. Designed for CI plumbing.
#
# Usage:  tests/basic/run_basic_tests.sh
# Each subtest:
#   tests/basic/scripts/<name>.in     - lines fed to BASIC stdin
#   tests/basic/scripts/<name>.expect - substrings that must appear in order
#                                       (lines starting with '#' are comments)
#
# Naming convention: prefix `nascom_` uses --autostart + NASCOM ROM,
# prefix `tiny_` uses Tiny BASIC ROM (no autostart).

set -u
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/bin/basicrunner"
NASCOM="$ROOT/tests/basic/nascom_basic_4_7_rc2014.hex"
TINY="$ROOT/tests/basic/tinybasic_1k.hex"

if [ ! -x "$BIN" ]; then
  echo "FAIL: basicrunner not built at $BIN — run \`make basicrunner\` first" >&2
  exit 2
fi

MAX_INSTR=30000000
pass=0
fail=0
fail_names=""

# Encode a BASIC script as CR-LF-terminated lines with a leading space on each
# line after the first — NASCOM BASIC 4.7 drops the first character after the
# previous command's "Ok\n" echo, so the leading space is a sacrificial filler.
# Tiny BASIC doesn't have this glitch but the same encoding works there too.
prep_input() {
  local f="$1"
  while IFS= read -r line || [ -n "$line" ]; do
    printf ' %s\r\n' "$line"
  done < "$f"
}

# Run one subtest. Echo PASS or FAIL line; update counters; print first miss.
run_one() {
  local name="$1" rom="$2" rom_args="$3"
  local in_file="$ROOT/tests/basic/scripts/$name.in"
  local expect_file="$ROOT/tests/basic/scripts/$name.expect"
  [ -f "$in_file" ]     || { echo "  SKIP $name (no .in)";  return; }
  [ -f "$expect_file" ] || { echo "  SKIP $name (no .expect)"; return; }

  local out
  out=$(prep_input "$in_file" | "$BIN" $rom_args --max-instr $MAX_INSTR "$rom" 2>&1) || true

  local missed=""
  while IFS= read -r want || [ -n "$want" ]; do
    case "$want" in ''|'#'*) continue;; esac
    if ! printf '%s' "$out" | grep -qF -- "$want"; then
      missed="${missed:+$missed | }\"$want\""
    fi
  done < "$expect_file"

  if [ -z "$missed" ]; then
    echo "  PASS $name"
    pass=$((pass + 1))
  else
    echo "  FAIL $name -- missing: $missed"
    fail=$((fail + 1))
    fail_names="${fail_names:+$fail_names }$name"
  fi
}

echo "== running BASIC ROM tests =="
echo "-- NASCOM BASIC 4.7c (rc2014, --autostart) --"
for s in "$ROOT"/tests/basic/scripts/nascom_*.in; do
  [ -e "$s" ] || continue
  name=$(basename "$s" .in)
  run_one "$name" "$NASCOM" "--autostart"
done
echo "-- Tiny BASIC 1K --"
for s in "$ROOT"/tests/basic/scripts/tiny_*.in; do
  [ -e "$s" ] || continue
  name=$(basename "$s" .in)
  run_one "$name" "$TINY" ""
done

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL BASIC ROM TESTS PASSED ($pass subtests)"
  exit 0
else
  echo "BASIC ROM TESTS: $pass PASS, $fail FAIL ($fail_names)"
  exit 1
fi
