#!/usr/bin/env python3
# ===========================================================================
# run_zex.py - drive a ZEX exerciser through the C model and report pass/fail.
#
#   run_zex.py <prelim|zexdoc|zexall> [max_instr]
#
# Runs build/bin/zexrunner on tests/zex/<which>.com, streams its output, and
# classifies the result: PASS only when the exerciser prints its completion
# banner with no "ERROR" line; a run that stops early (the instruction limit,
# or a jump to 0000 before the banner) fails.
# ===========================================================================
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "build", "bin", "zexrunner")

def main():
    # --runner EXE: another CP/M runner with the same output (make redcode_zex)
    args = sys.argv[1:]
    runner = RUNNER
    if args[:1] == ["--runner"]:
        runner, args = args[1], args[2:]
    which = args[0] if len(args) > 0 else "prelim"
    max_instr = args[1] if len(args) > 1 else "4000000000"
    com = os.path.join(ROOT, "tests", "zex", which + ".com")

    if not os.path.exists(runner):
        raise SystemExit(f"missing {runner} (run: make zexrunner)")
    if not os.path.exists(com):
        raise SystemExit(f"missing {com}")

    print(f"== running {which}.com through {'the C model' if runner == RUNNER else os.path.basename(runner)} ==")
    proc = subprocess.Popen([runner, com, max_instr], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1,
                            errors="replace")
    out_lines = []
    for line in proc.stdout:
        sys.stdout.write(line)
        out_lines.append(line)
    proc.wait()
    text = "".join(out_lines)

    err = "ERROR" in text                       # ZEX prints "ERROR" on CRC mismatch
    limit = "(LIMIT REACHED)" in text
    # ZEX prints a "...complete" banner when it finishes cleanly
    complete = "complete" in text.lower()

    print("\n---- run_zex summary ----")
    if err:
        print("FAIL: exerciser reported CRC mismatch(es)")
        return 1
    if limit:
        print(f"FAIL: instruction limit reached before {which} completed")
        return 3
    if complete and proc.returncode == 0:
        print(f"PASS: {which} completed with no errors")
        return 0
    print(f"FAIL: {which} ended without its completion banner (exit {proc.returncode})")
    return 4

if __name__ == "__main__":
    sys.exit(main())
