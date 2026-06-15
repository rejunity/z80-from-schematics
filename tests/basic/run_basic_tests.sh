#!/usr/bin/env bash
# Run NASCOM BASIC 4.7 + Tiny BASIC scripts through either the C model
# (basicrunner) or the Verilated RTL (sim_basic) and verify expected
# substrings appear in stdout. Exit 0 if all subtests pass; non-zero
# (and a printed list of failures) otherwise.
#
# Usage:  tests/basic/run_basic_tests.sh [c|rtl]   (default: c)
#
# Each subtest:
#   tests/basic/scripts/<name>.in     - lines fed to BASIC stdin
#   tests/basic/scripts/<name>.expect - substrings that must appear in order
#                                       (lines starting with '#' are comments)
#
# Naming convention: prefix `nascom_` uses --autostart + NASCOM ROM,
# prefix `tiny_` uses Tiny BASIC ROM (no autostart).

set -u
set -o pipefail

MODE="${1:-c}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NASCOM="$ROOT/tests/basic/nascom_basic_4_7_rc2014.hex"
TINY="$ROOT/tests/basic/tinybasic_1k.hex"

case "$MODE" in
  c)
    BIN="$ROOT/build/bin/basicrunner"
    SUITE_NAME="C model"
    if [ ! -x "$BIN" ]; then
      echo "FAIL: basicrunner not built at $BIN — run \`make basicrunner\` first" >&2
      exit 2
    fi
    ;;
  rtl)
    BIN="$ROOT/build/obj_dir_basic/sim_basic"
    SUITE_NAME="Verilator RTL"
    if [ ! -x "$BIN" ]; then
      echo "FAIL: sim_basic not built at $BIN — run \`make verilator_basic\` first" >&2
      exit 2
    fi
    ;;
  *)
    echo "usage: $0 [c|rtl]" >&2
    exit 2
    ;;
esac

# Verilator is ~20x slower than the C model on this workload, but we also
# don't need autostart's full prompt sequence — keep the same instr cap so
# every subtest's coverage is identical across the two harnesses. Real
# wall-clock per subtest: ~1 s on C, ~30 s on Verilator (CI Linux runner).
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

# Run one subtest. Echo subtest input + BASIC output for live CI visibility,
# then assert expected substrings. Updates counters.
run_one() {
  local name="$1" rom="$2" rom_args="$3"
  local in_file="$ROOT/tests/basic/scripts/$name.in"
  local expect_file="$ROOT/tests/basic/scripts/$name.expect"
  [ -f "$in_file" ]     || { echo "  SKIP $name (no .in)";  return; }
  [ -f "$expect_file" ] || { echo "  SKIP $name (no .expect)"; return; }

  echo
  echo "================  $name  ================"
  echo "----- input ($in_file) -----"
  cat "$in_file"
  echo "----- BASIC stdout -----"
  local tmplog="/tmp/basic_test_${name}_$$.log"
  rm -f "$tmplog"
  case "$MODE" in
    c)
      prep_input "$in_file" \
        | stdbuf -oL "$BIN" $rom_args --max-instr $MAX_INSTR "$rom" 2>&1 \
        | stdbuf -oL tee "$tmplog" || true
      ;;
    rtl)
      # sim_basic argv: [--autostart] <rom.hex> [max_instr]. Pass through
      # the same NASCOM --autostart flag so the "Memory top?" prompt is
      # skipped. NASCOM cold-start to first prompt is ~50M instr; each
      # PRINT subtest adds ~1M; bump the cap to 100M for comfort. Tiny
      # BASIC takes <5M so 100M is plenty there too.
      prep_input "$in_file" \
        | stdbuf -oL "$BIN" $rom_args "$rom" 100000000 2>&1 \
        | stdbuf -oL tee "$tmplog" || true
      ;;
  esac
  out=$(cat "$tmplog")
  rm -f "$tmplog"

  echo "----- assertions -----"
  local missed=""
  while IFS= read -r want || [ -n "$want" ]; do
    case "$want" in ''|'#'*) continue;; esac
    if printf '%s' "$out" | grep -qF -- "$want"; then
      echo "  OK    \"$want\""
    else
      echo "  MISS  \"$want\""
      missed="${missed:+$missed | }\"$want\""
    fi
  done < "$expect_file"

  if [ -z "$missed" ]; then
    echo "----- $name : PASS -----"
    pass=$((pass + 1))
  else
    echo "----- $name : FAIL (missing: $missed) -----"
    fail=$((fail + 1))
    fail_names="${fail_names:+$fail_names }$name"
  fi
}

echo "== running BASIC ROM tests ($SUITE_NAME) =="
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
